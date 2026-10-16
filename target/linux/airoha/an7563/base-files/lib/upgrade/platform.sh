# SPDX-License-Identifier: GPL-2.0-only

PART_NAME=firmware
REQUIRE_IMAGE_METADATA=1

platform_check_image() {
	case "$(board_name)" in
	xiaomi,be5000|airoha,an7563-evb)
		[ "$(get_magic_long "$1")" = "d00dfeed" ] && return 0
		echo "Expected an AN7563 FIT firmware image."
		return 1
		;;
	esac
	echo "Unsupported board."
	return 1
}

# The FIT stays raw in "firmware" (the bootloader reads it from there),
# the overlay is the rootfs_data volume in the "ubi" partition.
xiaomi_be5000_do_upgrade() {
	local ubidev

	get_image "$1" | mtd write - "$PART_NAME" || return 1

	ubidev="$(nand_find_ubi ubi)"
	if [ -z "$ubidev" ]; then
		# Not attached: preinit of the new image formats the partition
		v "no UBI on the ubi partition, the overlay starts empty"
		return 0
	fi

	[ -n "$(nand_find_volume "$ubidev" rootfs_data)" ] && \
		ubirmvol "/dev/$ubidev" -N rootfs_data
	ubimkvol "/dev/$ubidev" -N rootfs_data -m || return 1

	if [ -n "$UPGRADE_BACKUP" ]; then
		CI_UBIPART=ubi nand_restore_config "$UPGRADE_BACKUP"
	fi
	return 0
}

platform_do_upgrade() {
	case "$(board_name)" in
	xiaomi,be5000)
		xiaomi_be5000_do_upgrade "$1"
		;;
	*)
		default_do_upgrade "$1"
		;;
	esac
}

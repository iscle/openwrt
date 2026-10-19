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

	# Old kernels do not expose the new UBI partition. Boot the new
	# initramfs before migrating; never write an image we cannot provision.
	[ -n "$(find_mtd_index ubi)" ] || {
		echo "Boot the new initramfs before migrating to the UBI layout." >&2
		return 1
	}
	# An explicit upgrade may initialize the overlay partition. Normal
	# preinit only attempts attachment and never formats on an error.
	ubidev="$(nand_attach_ubi ubi)" || return 1
	[ -n "$ubidev" ] || return 1

	get_image "$1" | mtd write - "$PART_NAME" || return 1

	[ -n "$(nand_find_volume "$ubidev" rootfs_data)" ] && \
		{ ubirmvol "/dev/$ubidev" -N rootfs_data || return 1; }
	ubimkvol "/dev/$ubidev" -N rootfs_data -m || return 1

	if [ -n "$UPGRADE_BACKUP" ]; then
		CI_UBIPART=ubi nand_restore_config "$UPGRADE_BACKUP" || return 1
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

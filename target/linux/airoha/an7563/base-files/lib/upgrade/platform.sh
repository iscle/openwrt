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

platform_do_upgrade() {
	default_do_upgrade "$1"
}

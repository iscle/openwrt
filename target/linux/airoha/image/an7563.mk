define Target/Description
	Build firmware images for Airoha AN7563 (ARMv8 Cortex-A53 running
	in AArch32 mode) based boards.
endef

define Build/an7563-uboot
  cat $(STAGING_DIR_IMAGE)/an7563_$1-u-boot.bin >> $@
endef

# The EVB DTS is a bring-up skeleton with no validated Ethernet wiring.
# Keep it out of supported images until its network devices are described.

define Device/xiaomi_be5000
  DEVICE_VENDOR := Xiaomi
  DEVICE_MODEL := BE5000
  DEVICE_DTS := an7563-xiaomi-be5000
  DEVICE_PACKAGES := an7552-npu-firmware kmod-phy-airoha-en8811h \
	kmod-mt7996e kmod-mt7992-23-firmware wpad-basic-mbedtls iw
  DEVICE_COMPAT_VERSION := 1.1
  DEVICE_COMPAT_MESSAGE := UBI layout migration: RAM-boot the matching initramfs, then install with sysupgrade -n
  KERNEL_LOADADDR := 0x80088000
  # Must match fitsize in U-Boot's an7563 environment (0x500000).
  KERNEL_SIZE := 5120k
  IMAGE_SIZE := 24576k
  IMAGE/sysupgrade.bin := append-kernel | pad-to 128k | append-rootfs | \
	pad-rootfs | append-metadata | check-size
  ARTIFACT/u-boot.bin := an7563-uboot xiaomi_be5000
  ARTIFACTS := u-boot.bin
endef
TARGET_DEVICES += xiaomi_be5000

define Target/Description
	Build firmware images for Airoha AN7563 (ARMv8 Cortex-A53 running
	in AArch32 mode) based boards.
endef

define Build/an7563-uboot
  cat $(STAGING_DIR_IMAGE)/an7563_$1-u-boot.bin >> $@
endef

define Device/airoha_an7563-evb
  DEVICE_VENDOR := Airoha
  DEVICE_MODEL := AN7563 Evaluation Board
  DEVICE_DTS := an7563-evb
  KERNEL_LOADADDR := 0x80088000
  ARTIFACT/u-boot.bin := an7563-uboot rfb
  ARTIFACTS := u-boot.bin
endef
TARGET_DEVICES += airoha_an7563-evb

define Device/xiaomi_be5000
  DEVICE_VENDOR := Xiaomi
  DEVICE_MODEL := BE5000
  DEVICE_DTS := an7563-xiaomi-be5000
  DEVICE_PACKAGES := kmod-mdio-an7563 kmod-phy-airoha-en8811h \
	kmod-mt7996e kmod-mt7992-23-firmware wpad-basic-mbedtls iw
  KERNEL_LOADADDR := 0x80088000
  ARTIFACT/u-boot.bin := an7563-uboot xiaomi_be5000
  ARTIFACTS := u-boot.bin
endef
TARGET_DEVICES += xiaomi_be5000

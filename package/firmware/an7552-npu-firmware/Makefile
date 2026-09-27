# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=an7552-npu-firmware
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=COPYING
PKG_BUILD_DEPENDS:=riscv32-elf-gcc/host
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/an7552-npu-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Source-built AN7552 network processor firmware
  DEPENDS:=@TARGET_airoha_an7563
endef

define Package/an7552-npu-firmware/description
 Firmware for the two RISC-V network processor cores in the AN7552/AN7563.
 Implements MT7992 transmit, receive reordering, replay protection and
 hardware PPE forwarding. Built from source with a bare-metal RISC-V compiler.
endef

define Build/Configure
endef

define Build/Compile
	+$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR) \
		CROSS_COMPILE="$(STAGING_DIR_HOSTPKG)/riscv32-elf/bin/riscv32-unknown-elf-"
endef

define Package/an7552-npu-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/airoha
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/build/an7552.bin $(1)/lib/firmware/airoha/an7552.bin
endef

$(eval $(call BuildPackage,an7552-npu-firmware))

# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=riscv32-elf-binutils
PKG_VERSION:=2.46.1
PKG_SOURCE:=binutils-$(PKG_VERSION).tar.xz
PKG_SOURCE_URL:=@GNU/binutils
PKG_HASH:=e127a709cba24c76de8936cb7083dd768f28cd37eb010492e2f19b71eb1294e4
PKG_LICENSE:=GPL-3.0-or-later
PKG_CPE_ID:=cpe:/a:gnu:binutils

PKG_HOST_ONLY:=1
HOST_BUILD_PARALLEL:=1
HOST_BUILD_DIR:=$(BUILD_DIR_HOST)/$(PKG_NAME)-$(PKG_VERSION)
HOST_UNPACK=$(TAR) -C $(HOST_BUILD_DIR) --strip-components=1 -xf $(DL_DIR)/$(PKG_SOURCE)

include $(INCLUDE_DIR)/host-build.mk
include $(INCLUDE_DIR)/package.mk

HOST_CONFIGURE_ARGS = \
	--build=$(GNU_HOST_NAME) \
	--host=$(GNU_HOST_NAME) \
	--target=riscv32-unknown-elf \
	--prefix=$(STAGING_DIR_HOSTPKG)/riscv32-elf \
	--enable-deterministic-archives \
	--disable-nls \
	--disable-multilib \
	--disable-gdb \
	--disable-gprofng \
	--disable-sim \
	--disable-werror \
	--without-debuginfod \
	--without-zstd

HOST_MAKE_PATH:=build

define Host/Configure
	mkdir -p $(HOST_BUILD_DIR)/build
	(cd $(HOST_BUILD_DIR)/build; $(HOST_CONFIGURE_VARS) ../configure $(HOST_CONFIGURE_ARGS))
endef

define Host/Compile
	$(call Host/Compile/Default,all-binutils all-gas all-ld)
endef

define Host/Install
	$(call Host/Compile/Default,install-binutils install-gas install-ld)
endef

define Package/riscv32-elf-binutils
  SECTION:=devel
  CATEGORY:=Development
  TITLE:=Bare-metal RISC-V 32-bit binutils
  BUILDONLY:=1
endef

$(eval $(call BuildPackage,riscv32-elf-binutils))
$(eval $(call HostBuild))

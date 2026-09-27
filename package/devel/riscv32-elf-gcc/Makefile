# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=riscv32-elf-gcc
PKG_VERSION:=14.4.0
PKG_SOURCE:=gcc-$(PKG_VERSION).tar.xz
PKG_SOURCE_URL:=@GNU/gcc/gcc-$(PKG_VERSION)
PKG_HASH:=752b6f567beac83159c77a7680b1316bdd784738bff9a9d070112c09da90f6d9
PKG_LICENSE:=GPL-3.0-or-later
PKG_CPE_ID:=cpe:/a:gnu:gcc
HOST_BUILD_DEPENDS:=riscv32-elf-binutils/host

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
	--with-arch=rv32imc_zicsr_zifencei \
	--with-abi=ilp32 \
	--enable-languages=c \
	--disable-bootstrap \
	--disable-multilib \
	--disable-nls \
	--disable-shared \
	--disable-threads \
	--disable-libatomic \
	--disable-libgomp \
	--disable-libquadmath \
	--disable-libssp \
	--without-headers \
	--without-isl \
	--with-gmp=$(STAGING_DIR_HOST) \
	--with-mpfr=$(STAGING_DIR_HOST) \
	--with-mpc=$(STAGING_DIR_HOST)

HOST_MAKE_PATH:=build
HOST_CONFIGURE_VARS += PATH="$(STAGING_DIR_HOSTPKG)/riscv32-elf/bin:$(TARGET_PATH)"
HOST_MAKE_VARS += PATH="$(STAGING_DIR_HOSTPKG)/riscv32-elf/bin:$(TARGET_PATH)"

define Host/Configure
	mkdir -p $(HOST_BUILD_DIR)/build
	(cd $(HOST_BUILD_DIR)/build; $(HOST_CONFIGURE_VARS) ../configure $(HOST_CONFIGURE_ARGS))
endef

define Host/Compile
	$(call Host/Compile/Default,all-gcc)
endef

define Host/Install
	$(call Host/Compile/Default,install-gcc)
endef

define Package/riscv32-elf-gcc
  SECTION:=devel
  CATEGORY:=Development
  TITLE:=Bare-metal RISC-V 32-bit C compiler
  BUILDONLY:=1
endef

$(eval $(call BuildPackage,riscv32-elf-gcc))
$(eval $(call HostBuild))

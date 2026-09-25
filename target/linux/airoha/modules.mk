# SPDX-License-Identifier: GPL-2.0-only

OTHER_MENU:=Other modules


define KernelPackage/pwm-airoha
  SUBMENU:=$(OTHER_MENU)
  TITLE:=Airoha AN7581 and AN7583 PWM
  DEPENDS:=@TARGET_airoha_an7581||TARGET_airoha_an7583
  KCONFIG:= \
        CONFIG_PWM=y \
        CONFIG_PWM_AIROHA=y \
        CONFIG_PWM_SYSFS=y
  FILES:= \
        $(LINUX_DIR)/drivers/pwm/pwm-airoha.ko
  AUTOLOAD:=$(call AutoProbe,pwm-airoha)
endef

define KernelPackage/pwm-airoha/description
 Kernel module to use the PWM channel on Airoha SoC
endef

$(eval $(call KernelPackage,pwm-airoha))

define KernelPackage/mdio-an7563
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=Airoha AN7563 external PHY MDIO
  DEPENDS:=@TARGET_airoha_an7563 +kmod-libphy
  KCONFIG:=CONFIG_MDIO_AN7563
  FILES:=$(LINUX_DIR)/drivers/net/mdio/mdio-an7563.ko
  AUTOLOAD:=$(call AutoLoad,20,mdio-an7563,1)
endef

$(eval $(call KernelPackage,mdio-an7563))

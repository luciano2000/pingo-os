################################################################################
#
# h700-suspend-stub
#
################################################################################

# Mesmo commit usado pelo ROCKNIX
H700_SUSPEND_STUB_VERSION = 712653d11230b2450061ddb8e25f31e072da7a48
H700_SUSPEND_STUB_SITE = $(call github,MagnominusGit,h700-suspend-stub,$(H700_SUSPEND_STUB_VERSION))
H700_SUSPEND_STUB_LICENSE = GPL-2.0+
H700_SUSPEND_STUB_LICENSE_FILES = COPYING
H700_SUSPEND_STUB_INSTALL_TARGET = NO
H700_SUSPEND_STUB_INSTALL_IMAGES = YES

# Só o código-fonte (já com patches) do U-Boot e do TF-A: depender do build
# fecharia o ciclo u-boot -> tf-a -> stub -> u-boot.
H700_SUSPEND_STUB_PATCH_DEPENDENCIES = uboot arm-trusted-firmware

# Um stub por tipo de DRAM; por enquanto só LPDDR4 (RG35XX-H, RG40XX-H/V)
H700_SUSPEND_STUB_DEFCONFIG = \
	$(BR2_EXTERNAL_PINGO_PATH)/board/h700/uboot/anbernic_rg35xx_h700_lpddr4_defconfig

define H700_SUSPEND_STUB_BUILD_CMDS
	mkdir -p $(@D)/lpddr4
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/lpddr4 -f $(@D)/Makefile \
		SRC_DIR=$(@D) \
		UBOOT_DIR=$(UBOOT_DIR) \
		ATF_DIR=$(ARM_TRUSTED_FIRMWARE_DIR) \
		DEFCONFIG=$(H700_SUSPEND_STUB_DEFCONFIG) \
		OUT=suspend_stub_lpddr4.bin \
		CROSS_COMPILE=$(TARGET_CROSS)
endef

define H700_SUSPEND_STUB_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0644 $(@D)/suspend_stub_lpddr4.bin \
		$(BINARIES_DIR)/suspend_stub_lpddr4.bin
endef

$(eval $(generic-package))

include $(sort $(wildcard $(BR2_EXTERNAL_PINGO_PATH)/package/*/*.mk))

PINGO_H700_BOARD = $(BR2_EXTERNAL_PINGO_PATH)/board/h700

# --- Kernel ---------------------------------------------------------------
# Device trees do ROCKNIX (variantes de painel e de aparelho) entram na árvore
# do kernel, como o ROCKNIX faz com rsync em arch/arm64/boot/dts.
define PINGO_LINUX_ADD_DTS
	rsync -a $(PINGO_H700_BOARD)/dts/ $(LINUX_DIR)/arch/arm64/boot/dts/
	for f in $(PINGO_H700_BOARD)/dts/allwinner/*.dts; do \
		d=$$(basename $${f%.dts}).dtb; \
		grep -q "$$d" $(LINUX_DIR)/arch/arm64/boot/dts/allwinner/Makefile || \
			echo "dtb-\$$(CONFIG_ARCH_SUNXI) += $$d" >> $(LINUX_DIR)/arch/arm64/boot/dts/allwinner/Makefile; \
	done
endef
LINUX_POST_PATCH_HOOKS += PINGO_LINUX_ADD_DTS

# Firmwares embutidos no kernel (CONFIG_EXTRA_FIRMWARE): o painel e o Wi-Fi são
# drivers built-in e pedem o firmware antes de a raiz estar montada.
PINGO_LINUX_FW_WIFI = rtw88/rtw8821c_fw.bin rtl_bt/rtl8821cs_config.bin rtl_bt/rtl8821cs_fw.bin
define PINGO_LINUX_ADD_FIRMWARE
	mkdir -p $(LINUX_DIR)/external-firmware/panels
	cp $(PINGO_H700_BOARD)/firmware/panels/*.panel $(LINUX_DIR)/external-firmware/panels/
	for f in $(PINGO_LINUX_FW_WIFI); do \
		install -D -m 0644 "$$(readlink -f $(LINUX_FIRMWARE_DIR)/$$f)" $(LINUX_DIR)/external-firmware/$$f || exit 1; \
	done
endef
ifeq ($(BR2_PACKAGE_LINUX_FIRMWARE),y)
LINUX_DEPENDENCIES += linux-firmware
LINUX_PRE_BUILD_HOOKS += PINGO_LINUX_ADD_FIRMWARE
endif

# --- U-Boot ---------------------------------------------------------------
# defconfig do ROCKNIX para LPDDR4 (RG35XX-H, RG40XX-H/V)
define PINGO_UBOOT_ADD_DEFCONFIG
	cp $(PINGO_H700_BOARD)/uboot/*_defconfig $(UBOOT_DIR)/configs/
endef
UBOOT_POST_PATCH_HOOKS += PINGO_UBOOT_ADD_DEFCONFIG

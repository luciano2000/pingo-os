# --- Cores libretro ---------------------------------------------------------
# Modelo comum: cada pacote libretro-* define _CORE (nome do .so), _MAKEFILE,
# _SUBDIR (opcional) e _MAKE_OPTS (opcional) e chama este macro.
# Só CC/CXX/AR na linha de comando: os Makefiles dos cores acrescentam as
# próprias flags com +=, que um CFLAGS na linha de comando anularia.
define pingo-libretro-core
$(1)_BUILD_CMDS = $$(TARGET_MAKE_ENV) $$(MAKE) -C $$(@D)/$$($(1)_SUBDIR) \
	-f $$($(1)_MAKEFILE) platform=unix \
	CC="$$(TARGET_CC)" CXX="$$(TARGET_CXX)" AR="$$(TARGET_AR)" $$($(1)_MAKE_OPTS)
$(1)_INSTALL_TARGET_CMDS = $$(INSTALL) -D -m 0644 \
	$$(@D)/$$($(1)_SUBDIR)/$$($(1)_CORE)_libretro.so \
	$$(TARGET_DIR)/usr/lib/libretro/$$($(1)_CORE)_libretro.so
endef

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
# Alguns nomes são só "Link:" no WHENCE do linux-firmware; resolvemos aqui.
PINGO_LINUX_FW_WIFI = rtw88/rtw8821c_fw.bin rtl_bt/rtl8821cs_config.bin rtl_bt/rtl8821cs_fw.bin
define PINGO_LINUX_ADD_FIRMWARE
	mkdir -p $(LINUX_DIR)/external-firmware/panels
	cp $(PINGO_H700_BOARD)/firmware/panels/*.panel $(LINUX_DIR)/external-firmware/panels/
	for f in $(PINGO_LINUX_FW_WIFI); do \
		src=$(LINUX_FIRMWARE_DIR)/$$f; \
		if [ ! -e "$$src" ]; then \
			l=$$(sed -n "s|^Link: $$f -> ||p" $(LINUX_FIRMWARE_DIR)/WHENCE); \
			src=$(LINUX_FIRMWARE_DIR)/$$(dirname $$f)/$$l; \
		fi; \
		install -D -m 0644 "$$(readlink -f $$src)" $(LINUX_DIR)/external-firmware/$$f || exit 1; \
	done
endef
# Os .mk do Buildroot já foram avaliados quando este arquivo é lido, então
# acrescentar a LINUX_DEPENDENCIES não muda a ordem do build: a dependência
# entra direto na regra do passo de configuração.
ifeq ($(BR2_PACKAGE_LINUX_FIRMWARE),y)
$(LINUX_TARGET_CONFIGURE): | linux-firmware
LINUX_PRE_BUILD_HOOKS += PINGO_LINUX_ADD_FIRMWARE
endif

# --- U-Boot ---------------------------------------------------------------
# defconfig do ROCKNIX para LPDDR4 (RG35XX-H, RG40XX-H/V)
define PINGO_UBOOT_ADD_DEFCONFIG
	cp $(PINGO_H700_BOARD)/uboot/*_defconfig $(UBOOT_DIR)/configs/
endef
UBOOT_POST_PATCH_HOOKS += PINGO_UBOOT_ADD_DEFCONFIG

# --- TF-A -----------------------------------------------------------------
# Suspend profundo: o BL31 embute o stub de SRAM (PSCI SYSTEM_SUSPEND).
ifeq ($(BR2_PACKAGE_H700_SUSPEND_STUB),y)
ARM_TRUSTED_FIRMWARE_MAKE_OPTS += \
	SUNXI_SYSTEM_SUSPEND=1 \
	SUNXI_SUSPEND_STUB=$(BINARIES_DIR)/suspend_stub_lpddr4.bin
$(ARM_TRUSTED_FIRMWARE_TARGET_CONFIGURE): | h700-suspend-stub
endif

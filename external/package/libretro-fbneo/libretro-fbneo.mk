################################################################################
#
# libretro-fbneo
#
################################################################################

LIBRETRO_FBNEO_VERSION = 72af9e23b766b6ad6d2a30042fc8d83eadd25b61
LIBRETRO_FBNEO_SITE = $(call github,libretro,FBNeo,$(LIBRETRO_FBNEO_VERSION))
LIBRETRO_FBNEO_LICENSE = FBNeo (não comercial)
LIBRETRO_FBNEO_LICENSE_FILES = LICENSE.md
LIBRETRO_FBNEO_CORE = fbneo
LIBRETRO_FBNEO_SUBDIR = src/burner/libretro
LIBRETRO_FBNEO_MAKEFILE = Makefile

$(eval $(call pingo-libretro-core,LIBRETRO_FBNEO))

# Nomes legíveis do arcade: o DAT do romset desta versão vira "zip<TAB>nome<TAB>bios"
define LIBRETRO_FBNEO_ARCADE_NAMES
	mkdir -p $(TARGET_DIR)/usr/share/pingo/arcade
	python3 -I $(BR2_EXTERNAL_PINGO_PATH)/../scripts/fbneo-names.py \
		"$(@D)/dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat" \
		$(TARGET_DIR)/usr/share/pingo/arcade/fbneo.tsv
	# Banco de endereços dos recordes (o pingo-storage copia para Bios/fbneo/)
	$(INSTALL) -D -m 0644 $(@D)/metadata/hiscore.dat \
		$(TARGET_DIR)/usr/share/pingo/arcade/hiscore.dat
endef
LIBRETRO_FBNEO_POST_INSTALL_TARGET_HOOKS += LIBRETRO_FBNEO_ARCADE_NAMES

# Pingo Arcade: tabela da galeria (ano, fabricante, clone, sistema, tipo,
# gênero, jogadores, vertical)
ifeq ($(BR2_PACKAGE_RPI_FIRMWARE),y)
define LIBRETRO_FBNEO_ARCADE_GALLERY
	python3 -I $(BR2_EXTERNAL_PINGO_PATH)/../scripts/fbneo-arcade.py \
		"$(@D)/dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat" \
		$(@D)/src/burn $(TARGET_DIR)/usr/share/pingo/arcade/arcade.tsv
endef
LIBRETRO_FBNEO_POST_INSTALL_TARGET_HOOKS += LIBRETRO_FBNEO_ARCADE_GALLERY
endif

$(eval $(generic-package))

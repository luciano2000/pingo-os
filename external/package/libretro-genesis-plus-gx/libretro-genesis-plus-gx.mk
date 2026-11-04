################################################################################
#
# libretro-genesis-plus-gx
#
################################################################################

LIBRETRO_GENESIS_PLUS_GX_VERSION = f27655b592ef43f317272d79883bcaa10d3ac8ad
LIBRETRO_GENESIS_PLUS_GX_SITE = $(call github,libretro,Genesis-Plus-GX,$(LIBRETRO_GENESIS_PLUS_GX_VERSION))
LIBRETRO_GENESIS_PLUS_GX_LICENSE = Genesis Plus GX (não comercial)
LIBRETRO_GENESIS_PLUS_GX_LICENSE_FILES = LICENSE.txt
LIBRETRO_GENESIS_PLUS_GX_CORE = genesis_plus_gx
LIBRETRO_GENESIS_PLUS_GX_MAKEFILE = Makefile.libretro

$(eval $(call pingo-libretro-core,LIBRETRO_GENESIS_PLUS_GX))
$(eval $(generic-package))

################################################################################
#
# libretro-snes9x
#
################################################################################

LIBRETRO_SNES9X_VERSION = fae2fea08f74180759ef540ee94259213f503480
LIBRETRO_SNES9X_SITE = $(call github,libretro,snes9x,$(LIBRETRO_SNES9X_VERSION))
LIBRETRO_SNES9X_LICENSE = Snes9x (não comercial)
LIBRETRO_SNES9X_LICENSE_FILES = LICENSE
LIBRETRO_SNES9X_CORE = snes9x
LIBRETRO_SNES9X_MAKEFILE = Makefile
LIBRETRO_SNES9X_SUBDIR = libretro

$(eval $(call pingo-libretro-core,LIBRETRO_SNES9X))
$(eval $(generic-package))

################################################################################
#
# libretro-pokemini
#
################################################################################

LIBRETRO_POKEMINI_VERSION = 132111b76343559860532a1ccc094f93f1ed5650
LIBRETRO_POKEMINI_SITE = $(call github,libretro,PokeMini,$(LIBRETRO_POKEMINI_VERSION))
LIBRETRO_POKEMINI_LICENSE = GPL-3.0
LIBRETRO_POKEMINI_LICENSE_FILES = LICENSE
LIBRETRO_POKEMINI_CORE = pokemini
LIBRETRO_POKEMINI_MAKEFILE = Makefile.libretro

$(eval $(call pingo-libretro-core,LIBRETRO_POKEMINI))
$(eval $(generic-package))

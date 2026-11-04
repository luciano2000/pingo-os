################################################################################
#
# libretro-beetle-pce-fast
#
################################################################################

LIBRETRO_BEETLE_PCE_FAST_VERSION = 2de1ce1c3f237b161c126c54ba407990bb86d1e9
LIBRETRO_BEETLE_PCE_FAST_SITE = $(call github,libretro,beetle-pce-fast-libretro,$(LIBRETRO_BEETLE_PCE_FAST_VERSION))
LIBRETRO_BEETLE_PCE_FAST_LICENSE = GPL-2.0
LIBRETRO_BEETLE_PCE_FAST_LICENSE_FILES = COPYING
LIBRETRO_BEETLE_PCE_FAST_CORE = mednafen_pce_fast
LIBRETRO_BEETLE_PCE_FAST_MAKEFILE = Makefile

$(eval $(call pingo-libretro-core,LIBRETRO_BEETLE_PCE_FAST))
$(eval $(generic-package))

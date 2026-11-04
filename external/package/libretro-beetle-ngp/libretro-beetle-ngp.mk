################################################################################
#
# libretro-beetle-ngp
#
################################################################################

LIBRETRO_BEETLE_NGP_VERSION = a50d5ac288a81f2104ddf43195a4efdd15c72227
LIBRETRO_BEETLE_NGP_SITE = $(call github,libretro,beetle-ngp-libretro,$(LIBRETRO_BEETLE_NGP_VERSION))
LIBRETRO_BEETLE_NGP_LICENSE = GPL-2.0
LIBRETRO_BEETLE_NGP_LICENSE_FILES = COPYING
LIBRETRO_BEETLE_NGP_CORE = mednafen_ngp
LIBRETRO_BEETLE_NGP_MAKEFILE = Makefile

$(eval $(call pingo-libretro-core,LIBRETRO_BEETLE_NGP))
$(eval $(generic-package))

################################################################################
#
# libretro-handy
#
################################################################################

LIBRETRO_HANDY_VERSION = bc55d462f0b2d6b073ea93dc552ebd73cec60fd1
LIBRETRO_HANDY_SITE = $(call github,libretro,libretro-handy,$(LIBRETRO_HANDY_VERSION))
LIBRETRO_HANDY_LICENSE = Zlib
LIBRETRO_HANDY_LICENSE_FILES = lynx/license.txt
LIBRETRO_HANDY_CORE = handy
LIBRETRO_HANDY_MAKEFILE = Makefile

$(eval $(call pingo-libretro-core,LIBRETRO_HANDY))
$(eval $(generic-package))

################################################################################
#
# libretro-beetle-vb
#
################################################################################

LIBRETRO_BEETLE_VB_VERSION = d5ae54c8e23b573a3b34e888b2d48a588027e92c
LIBRETRO_BEETLE_VB_SITE = $(call github,libretro,beetle-vb-libretro,$(LIBRETRO_BEETLE_VB_VERSION))
LIBRETRO_BEETLE_VB_LICENSE = GPL-2.0
LIBRETRO_BEETLE_VB_LICENSE_FILES = COPYING
LIBRETRO_BEETLE_VB_CORE = mednafen_vb
LIBRETRO_BEETLE_VB_MAKEFILE = Makefile

$(eval $(call pingo-libretro-core,LIBRETRO_BEETLE_VB))
$(eval $(generic-package))

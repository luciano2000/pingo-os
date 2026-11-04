################################################################################
#
# libretro-bluemsx
#
################################################################################

LIBRETRO_BLUEMSX_VERSION = e3086eb5d36d77fa11704cf53dc176686e70127d
LIBRETRO_BLUEMSX_SITE = $(call github,libretro,blueMSX-libretro,$(LIBRETRO_BLUEMSX_VERSION))
LIBRETRO_BLUEMSX_LICENSE = blueMSX (não comercial)
LIBRETRO_BLUEMSX_LICENSE_FILES = license.txt
LIBRETRO_BLUEMSX_CORE = bluemsx
LIBRETRO_BLUEMSX_MAKEFILE = Makefile.libretro

$(eval $(call pingo-libretro-core,LIBRETRO_BLUEMSX))
$(eval $(generic-package))

################################################################################
#
# libretro-pcsx-rearmed
#
################################################################################

LIBRETRO_PCSX_REARMED_VERSION = 7b0e8f8fadfbce3d170d195b9d9eecaabfa08c8e
LIBRETRO_PCSX_REARMED_SITE = $(call github,libretro,pcsx_rearmed,$(LIBRETRO_PCSX_REARMED_VERSION))
LIBRETRO_PCSX_REARMED_LICENSE = GPL-2.0
LIBRETRO_PCSX_REARMED_LICENSE_FILES = COPYING
LIBRETRO_PCSX_REARMED_CORE = pcsx_rearmed
LIBRETRO_PCSX_REARMED_MAKEFILE = Makefile.libretro
LIBRETRO_PCSX_REARMED_MAKE_OPTS = ARCH=arm64 DYNAREC=ari64

$(eval $(call pingo-libretro-core,LIBRETRO_PCSX_REARMED))
$(eval $(generic-package))

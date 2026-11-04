################################################################################
#
# libretro-fceumm
#
################################################################################

LIBRETRO_FCEUMM_VERSION = e0f3086a8104f8fbbe49a2615b10401504a9533f
LIBRETRO_FCEUMM_SITE = $(call github,libretro,libretro-fceumm,$(LIBRETRO_FCEUMM_VERSION))
LIBRETRO_FCEUMM_LICENSE = GPL-2.0
LIBRETRO_FCEUMM_LICENSE_FILES = Copying
LIBRETRO_FCEUMM_CORE = fceumm
LIBRETRO_FCEUMM_MAKEFILE = Makefile.libretro

$(eval $(call pingo-libretro-core,LIBRETRO_FCEUMM))
$(eval $(generic-package))

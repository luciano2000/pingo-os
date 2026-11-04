################################################################################
#
# libretro-picodrive
#
################################################################################

LIBRETRO_PICODRIVE_VERSION = 1890c2932234c9d30f4cd3851d02228baae8f09e
# Clone com submódulos (libpicofe, cyclone, libchdr...): o .tar.gz do GitHub
# não traz os submódulos
LIBRETRO_PICODRIVE_SITE = https://github.com/libretro/picodrive.git
LIBRETRO_PICODRIVE_SITE_METHOD = git
LIBRETRO_PICODRIVE_GIT_SUBMODULES = YES
LIBRETRO_PICODRIVE_LICENSE = PicoDrive (não comercial)
LIBRETRO_PICODRIVE_LICENSE_FILES = COPYING
LIBRETRO_PICODRIVE_CORE = picodrive
LIBRETRO_PICODRIVE_MAKEFILE = Makefile.libretro

$(eval $(call pingo-libretro-core,LIBRETRO_PICODRIVE))
$(eval $(generic-package))

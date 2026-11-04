################################################################################
#
# libretro-beetle-wswan
#
################################################################################

LIBRETRO_BEETLE_WSWAN_VERSION = 4b01295838ea89e3f1355bbe4cb5cf98aa6108cd
LIBRETRO_BEETLE_WSWAN_SITE = $(call github,libretro,beetle-wswan-libretro,$(LIBRETRO_BEETLE_WSWAN_VERSION))
LIBRETRO_BEETLE_WSWAN_LICENSE = GPL-2.0
LIBRETRO_BEETLE_WSWAN_LICENSE_FILES = COPYING
LIBRETRO_BEETLE_WSWAN_CORE = mednafen_wswan
LIBRETRO_BEETLE_WSWAN_MAKEFILE = Makefile

$(eval $(call pingo-libretro-core,LIBRETRO_BEETLE_WSWAN))
$(eval $(generic-package))

################################################################################
#
# pingo-retro
#
################################################################################

PINGO_RETRO_VERSION = 0.1
PINGO_RETRO_SITE = $(BR2_EXTERNAL_PINGO_PATH)/../frontend
PINGO_RETRO_SITE_METHOD = local
PINGO_RETRO_LICENSE = GPL-2.0, MIT (libretro.h)
PINGO_RETRO_DEPENDENCIES = sdl2 host-pkgconf

define PINGO_RETRO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

define PINGO_RETRO_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))

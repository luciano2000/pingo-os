################################################################################
#
# pingo-frontend
#
################################################################################

PINGO_FRONTEND_VERSION = 0.2
PINGO_FRONTEND_SITE = $(BR2_EXTERNAL_PINGO_PATH)/../frontend
PINGO_FRONTEND_SITE_METHOD = local
# Binários compilados no PC (make para conferir o código) não podem entrar:
# mais novos que o código, o make do build os aceitaria como prontos e a
# imagem levaria um programa x86-64
PINGO_FRONTEND_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--exclude=/pingo-retro --exclude=/pingo-launcher --exclude='*.o'
PINGO_FRONTEND_LICENSE = GPL-2.0, MIT (libretro.h)
PINGO_FRONTEND_DEPENDENCIES = sdl2 sdl2_ttf font-inter alsa-lib libzip host-pkgconf \
	rcheevos libcurl

define PINGO_FRONTEND_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		RC_CFLAGS="-DPINGO_RA -DRC_CLIENT_SUPPORTS_HASH -DRC_STATIC -I$(STAGING_DIR)/usr/include/rcheevos" \
		RC_LIBS="-lrcheevos -lpthread"
endef

define PINGO_FRONTEND_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))

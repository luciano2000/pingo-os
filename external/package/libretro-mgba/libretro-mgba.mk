################################################################################
#
# libretro-mgba
#
################################################################################

LIBRETRO_MGBA_VERSION = 7a12d6d4b9acb14c0ae62c9166b6a2f3d08007f6
LIBRETRO_MGBA_SITE = $(call github,libretro,mgba,$(LIBRETRO_MGBA_VERSION))
LIBRETRO_MGBA_LICENSE = MPL-2.0
LIBRETRO_MGBA_LICENSE_FILES = LICENSE

# Só o core libretro, sem interface, scripting nem dependências externas
LIBRETRO_MGBA_CONF_OPTS = \
	-DBUILD_LIBRETRO=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_GL=OFF \
	-DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DBUILD_SHARED=OFF -DBUILD_STATIC=OFF \
	-DSKIP_LIBRARY=ON -DUSE_LUA=OFF -DUSE_FFMPEG=OFF -DUSE_ZLIB=OFF \
	-DUSE_PNG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_SQLITE3=OFF \
	-DUSE_ELF=OFF -DUSE_LZMA=OFF -DUSE_EDITLINE=OFF -DUSE_DISCORD_RPC=OFF \
	-DUSE_JSON_C=OFF -DENABLE_SCRIPTING=OFF -DUSE_DEBUGGERS=OFF

define LIBRETRO_MGBA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/mgba_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mgba_libretro.so
endef

$(eval $(cmake-package))

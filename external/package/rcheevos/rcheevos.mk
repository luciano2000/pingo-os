################################################################################
#
# rcheevos
#
################################################################################

RCHEEVOS_VERSION = 12.5.0
RCHEEVOS_SITE = $(call github,RetroAchievements,rcheevos,v$(RCHEEVOS_VERSION))
RCHEEVOS_LICENSE = MIT
RCHEEVOS_LICENSE_FILES = LICENSE
RCHEEVOS_INSTALL_STAGING = YES
RCHEEVOS_INSTALL_TARGET = NO

RCHEEVOS_CFLAGS = $(TARGET_CFLAGS) -DRC_CLIENT_SUPPORTS_HASH -DRC_STATIC \
	-Iinclude -Isrc

# Tudo menos o editor/RAIntegration (Windows) e o cliente "externo".
# rc_libretro.c inclui <libretro.h>: usa o mesmo do frontend.
define RCHEEVOS_BUILD_CMDS
	cp $(BR2_EXTERNAL_PINGO_PATH)/../frontend/include/libretro.h $(@D)/include/
	cd $(@D) && rm -f *.o librcheevos.a && \
	for f in src/*.c src/rapi/*.c src/rcheevos/*.c src/rhash/*.c; do \
		case $$f in */rc_client_external.c|*/rc_client_raintegration.c) continue ;; esac; \
		$(TARGET_CC) $(RCHEEVOS_CFLAGS) -c $$f -o $$(echo $$f | tr / _ | sed 's/\.c$$/.o/') || exit 1; \
	done && \
	$(TARGET_AR) rcs librcheevos.a *.o
endef

define RCHEEVOS_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/librcheevos.a $(STAGING_DIR)/usr/lib/librcheevos.a
	mkdir -p $(STAGING_DIR)/usr/include/rcheevos
	$(INSTALL) -m 0644 $(@D)/include/*.h $(@D)/src/rc_libretro.h $(STAGING_DIR)/usr/include/rcheevos/
endef

$(eval $(generic-package))

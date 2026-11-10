################################################################################
#
# libchdr
#
################################################################################

LIBCHDR_VERSION = 47d794151b2b64f6aba60304e6480f4f5a2bc24b
LIBCHDR_SITE = $(call github,rtissera,libchdr,$(LIBCHDR_VERSION))
LIBCHDR_LICENSE = BSD-3-Clause
LIBCHDR_LICENSE_FILES = LICENSE.txt
LIBCHDR_INSTALL_STAGING = YES
LIBCHDR_INSTALL_TARGET = NO

# unity.c junta quase tudo (a biblioteca e o LZMA, miniz e zstd) num
# arquivo só, sem CMake; o codec avhuff (vídeo, nunca usado em CD) ficou de
# fora dele, mas a tabela de codecs aponta para ele
LIBCHDR_CC = $(TARGET_CC) $(TARGET_CFLAGS) -fPIC -Iinclude \
	-Ideps/lzma-26.02/include -Ideps/zstd-1.5.7 -Ideps/miniz-3.1.2

define LIBCHDR_BUILD_CMDS
	cd $(@D) && $(LIBCHDR_CC) -c unity.c -o unity.o && \
	$(LIBCHDR_CC) -c src/libchdr_codec_avhuff.c -o avhuff.o && \
	rm -f libchdr.a && $(TARGET_AR) rcs libchdr.a unity.o avhuff.o
endef

define LIBCHDR_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/libchdr.a $(STAGING_DIR)/usr/lib/libchdr.a
	mkdir -p $(STAGING_DIR)/usr/include/libchdr
	$(INSTALL) -m 0644 $(@D)/include/libchdr/*.h $(STAGING_DIR)/usr/include/libchdr/
endef

$(eval $(generic-package))

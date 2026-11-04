################################################################################
#
# font-inter
#
################################################################################

FONT_INTER_VERSION = 4.1
FONT_INTER_SOURCE = Inter-$(FONT_INTER_VERSION).zip
FONT_INTER_SITE = https://github.com/rsms/inter/releases/download/v$(FONT_INTER_VERSION)
FONT_INTER_LICENSE = OFL-1.1
FONT_INTER_LICENSE_FILES = LICENSE.txt

define FONT_INTER_EXTRACT_CMDS
	$(UNZIP) -o -d $(@D) $(FONT_INTER_DL_DIR)/$(FONT_INTER_SOURCE) \
		LICENSE.txt extras/ttf/Inter-Regular.ttf \
		extras/ttf/Inter-Medium.ttf extras/ttf/Inter-SemiBold.ttf
endef

define FONT_INTER_INSTALL_TARGET_CMDS
	for w in Regular Medium SemiBold; do \
		$(INSTALL) -D -m 0644 $(@D)/extras/ttf/Inter-$$w.ttf \
			$(TARGET_DIR)/usr/share/fonts/inter/Inter-$$w.ttf || exit 1; \
	done
	$(INSTALL) -D -m 0644 $(@D)/LICENSE.txt $(TARGET_DIR)/usr/share/fonts/inter/LICENSE.txt
endef

$(eval $(generic-package))

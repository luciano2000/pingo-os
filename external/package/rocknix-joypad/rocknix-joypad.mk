################################################################################
#
# rocknix-joypad
#
################################################################################

ROCKNIX_JOYPAD_VERSION = 005033d1ddd8746c771f2c81f1174f2d892541c9
ROCKNIX_JOYPAD_SITE = $(call github,hackbar,rocknix-joypad,$(ROCKNIX_JOYPAD_VERSION))
ROCKNIX_JOYPAD_LICENSE = GPL-2.0+

$(eval $(kernel-module))
$(eval $(generic-package))

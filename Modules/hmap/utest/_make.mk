###############################################################################
#
#  /utest/_make.mk
#
#  hmap Unit Testing Definitions
#
###############################################################################
UMODULE := hmap
UMODULE_SUBDIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(BUILDER)/utest.mk


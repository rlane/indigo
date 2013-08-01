###############################################################################
#
#  /module/make.mk
#
#  hmap public includes are defined here
#
###############################################################################
THISDIR := $(dir $(lastword $(MAKEFILE_LIST)))
hmap_INCLUDES := -I $(THISDIR)inc
hmap_INTERNAL_INCLUDES := -I $(THISDIR)src


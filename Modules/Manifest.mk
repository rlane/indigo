
##############################################################################
#
# Builder Module Manifest.
#
# Autogenerated 2013-07-31 17:17:57.694593
#
##############################################################################
BASEDIR := $(dir $(lastword $(MAKEFILE_LIST)))
murmur_BASEDIR := $(BASEDIR)murmur
loci_BASEDIR := $(BASEDIR)loci
OFStateManager_BASEDIR := $(BASEDIR)Indigo/OFStateManager
Configuration_BASEDIR := $(BASEDIR)Indigo/Configuration
SocketManager_BASEDIR := $(BASEDIR)Indigo/SocketManager
OFConnectionManager_BASEDIR := $(BASEDIR)Indigo/OFConnectionManager
indigo_BASEDIR := $(BASEDIR)Indigo/indigo
cjson_BASEDIR := $(BASEDIR)cjson
hmap_BASEDIR := $(BASEDIR)hmap
BigList_BASEDIR := $(BASEDIR)BigData/BigList
AIM_BASEDIR := $(BASEDIR)AIM


ALL_MODULES := $(ALL_MODULES) indigo SocketManager cjson loci OFStateManager hmap OFConnectionManager AIM murmur BigList Configuration

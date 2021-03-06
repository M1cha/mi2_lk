# top level project rules for the msm8960_virtio project
#
LOCAL_DIR := $(GET_LOCAL_DIR)

TARGET := msm8960

MODULES += app/aboot

DEBUG := 0
EMMC_BOOT := 1

#DEFINES += WITH_DEBUG_DCC=1
#DEFINES += WITH_DEBUG_UART=1
#DEFINES += WITH_DEBUG_FBCON=1
ENABLE_THUMB := false

ifeq ($(EMMC_BOOT),1)
DEFINES += _EMMC_BOOT=1
endif

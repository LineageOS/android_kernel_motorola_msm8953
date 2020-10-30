# Android makefile for the ISDBT Module

LOCAL_PATH := $(call my-dir)

DLKM_DIR := $(TOP)/device/qcom/common/dlkm

# Build isdbt.ko
###########################################################
include $(CLEAR_VARS)
LOCAL_MODULE              := isdbt.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := false
include $(DLKM_DIR)/AndroidKernelModule.mk
###########################################################

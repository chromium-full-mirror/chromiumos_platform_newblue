LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_C_INCLUDES += \
	system/media/audio_utils/include \
	external/tinyalsa/include \
	$(LOCAL_PATH)/..

LOCAL_CFLAGS :=  -Wno-unused-parameter -DANDROID -DSG_DEBUG -DRFC_DEBUG #-DMT_DEBUG -DSDP_DBG -DTEST_DEBUG -DEXECUTABLE
LOCAL_SRC_FILES += a2dp_test.c audio.cpp DataPump.cpp
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libtinyalsa libaudioutils libcutils libdl liblog
LOCAL_STATIC_LIBRARIES := libNewBlue

LOCAL_MODULE := a2dp_test

include $(BUILD_EXECUTABLE)

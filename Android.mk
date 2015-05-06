LOCAL_PATH := $(call my-dir)

COMMON_SRC_FILES :=     \
    sg.c                \
    hci.c               \
    l2cap.c             \
    timer.c             \
    uniq.c              \
    vendorLib.c         \
    workQueue.c         \
    multiNotif.c        \
    sdp.c               \
    sendQ.c             \
    rfcomm.c            \
    mt.c                \
    sm.c                \
    persist.c           \
    uuid.c              \
    att.c               \
    gatt.c              \
    gatt-builtin.c      \
    a2dp.c              \

COMMON_CFLAGS += -Wno-unused-parameter -DANDROID #-DTEST_DEBUG -DSG_DEBUG -DRFC_DEBUG -DMT_DEBUG -DSDP_DBG -DA2DP_DBG
COMMON_SHARED_LIBRARIES := libdl liblog

### HAL shared library ###

include $(CLEAR_VARS)

#for shared lib
LOCAL_SRC_FILES := $(COMMON_SRC_FILES)
LOCAL_CFLAGS := $(COMMON_CFLAGS)
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libdl liblog

LOCAL_MODULE := bluetooth.NewBlue
LOCAL_SRC_FILES += aapi.c aapiSocket.c aapiGatt.c aapiGattClient.c aapiGattServer.c
LOCAL_MODULE_RELATIVE_PATH := hw
include $(BUILD_SHARED_LIBRARY)


### static library for executable test programs ###

include $(CLEAR_VARS)

LOCAL_SRC_FILES := $(COMMON_SRC_FILES)
LOCAL_CFLAGS := $(COMMON_CFLAGS)
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := $(COMMON_SHARED_LIBRARIES)

LOCAL_MODULE := libNewBlue
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

### executable test program ###

LOCAL_CFLAGS := $(COMMON_CFLAGS)
LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := $(COMMON_SRC_FILES)
LOCAL_SRC_FILES += test.c
LOCAL_MODULE := new_blue_test
LOCAL_CFLAGS += -DEXECUTABLE -O0 -g
LOCAL_SHARED_LIBRARIES += libdl liblog
include $(BUILD_EXECUTABLE)

# build tests in subdirectories
include $(call all-makefiles-under,$(LOCAL_PATH))

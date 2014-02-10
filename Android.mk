LOCAL_PATH := $(my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := new_blue_test
LOCAL_SRC_FILES := hci.c l2cap.c timer.c uniq.c vendorLib.c workQueue.c util.c multiNotif.c test.c
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += -Wno-unused-parameter
LOCAL_SHARED_LIBRARIES := libdl
include $(BUILD_EXECUTABLE)

LOCAL_PATH := $(my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := new_blue_test
LOCAL_SRC_FILES := sg.c hci.c l2cap.c timer.c uniq.c vendorLib.c workQueue.c multiNotif.c test.c sdp.c sendQ.c stack.c rfcomm.c mt.c sm.c
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += -Wno-unused-parameter #-DMT_DEBUG -DSG_DEBUG -DRFC_DEBUG -DTEST_DEBUG
LOCAL_SHARED_LIBRARIES := libdl
include $(BUILD_EXECUTABLE)

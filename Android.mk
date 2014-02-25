LOCAL_PATH := $(my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := new_blue_test
LOCAL_SRC_FILES := sg.c hci.c l2cap.c timer.c uniq.c vendorLib.c workQueue.c util.c multiNotif.c test.c sdp.c sendQ.c stack.c #profiles/RFCOMM/rfcomm.c
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += -Wno-unused-parameter
LOCAL_SHARED_LIBRARIES := libdl
include $(BUILD_EXECUTABLE)

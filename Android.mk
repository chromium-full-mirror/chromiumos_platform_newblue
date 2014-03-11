LOCAL_PATH := $(my-dir)
include $(CLEAR_VARS)


LOCAL_SRC_FILES := sg.c hci.c l2cap.c timer.c uniq.c vendorLib.c workQueue.c multiNotif.c sdp.c sendQ.c stack.c rfcomm.c mt.c sm.c
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += -Wno-unused-parameter #-DMT_DEBUG -DSG_DEBUG -DRFC_DEBUG -DTEST_DEBUG
LOCAL_SHARED_LIBRARIES := libdl

#for shared lib
LOCAL_MODULE := bluetooth.NewBlue
LOCAL_SRC_FILES += aapi.c aapiSocket.c aapiGatt.c aapiGattClient.c aapiGattServer.c
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SHARED_LIBRARIES += liblog
include $(BUILD_SHARED_LIBRARY)
#end
#for executable
#LOCAL_SRC_FILES += test.c
#LOCAL_MODULE := new_blue_test
#LOCAL_CFLAGS += -DEXECUTABLE
#include $(BUILD_EXECUTABLE)
#endif

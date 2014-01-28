//todo: BT_VND_OP_SCO_CFG
//todo: BT_VND_OP_GET_LPM_IDLE_TIMEOUT
//todo: BT_VND_OP_LPM_SET_MODE
//todo: BT_VND_OP_LPM_WAKE_SET_STATE

#include <semaphore.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include "bt_vendor_lib.h"
#include "vendorLib.h"
#include "config.h"
#include "l2cap.h"
#include "util.h"
#include "uniq.h"
#include "hci.h"
#include "log.h"

#define BT_VENDOR_LIB_NAME       "libbt-vendor.so"
#define BT_VENDOR_INFACE_STRUCT  "BLUETOOTH_VENDOR_LIB_INTERFACE"




/* our state w.r.t. vendor lib */
static void *mVendorLibHandle = NULL;
static const bt_vendor_interface_t *mVendorIface = NULL;
static vendorCbkRx mCbkRx;
static void* mCbkData;
static int mFdCmdOut;
static int mFdEvtIn;
static int mFdAclIn;
static int mFdAclOut;
static bool mCbkResult;
static sem_t mInitSem;
static pthread_t mWorkerThread;
static int mWorkerClosePipe;




/*
 * FUNCTION: vcbkFwCfg
 * USE:      Called when FW upload finishes
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:    Only needs to be done once.
 *           Keep it open from then on.
 */
static void vcbkFwCfg(bt_vendor_op_result_t result)
{
    mCbkResult = result == BT_VND_OP_RESULT_SUCCESS;
    sem_post(&mInitSem);
}

static void vcbkScoCfg(bt_vendor_op_result_t result)
{
    //todo
}

static void vcbkLbm(bt_vendor_op_result_t result)
{
    //todo
}

static void* vcbkAlloc(int size)
{
    //todo
}

static void vcbkDealloc(void *p_buf)
{
    //todo
}

static uint8_t vcbkXmit(uint16_t opcode, void *p_buf, tINT_CMD_CBACK p_cback)
{
    //todo
}

static void vcbkEpilog(bt_vendor_op_result_t result)
{
    logi("Epilog done\n");
    sem_post(&mInitSem);
}

/*
 * FUNCTION: vendorWorker
 * USE:      Vendor lib worker thread
 * PARAMS:   pipe - actually an int - the fd with th epipe used to tell us to quit
 * RETURN:   unused
 * NOTES:
 */
static void* vendorWorker(void *pipeP)
{
    int pipeFd = (int)(uintptr_t)pipeP;

    while (1) {
        struct pollfd pollfds[MAX_CH + 1];
        int numfds = 0;

        //add pipe, add others, wait, see...

    }
}

/*
 * FUNCTION: vendorOpen
 * USE:      Open the vendor library
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:    Only needs to be done once.
 *           Keep it open from then on.
 */
bool vendorOpen(void)
{
    if (mVendorLibHandle) {
        loge("Vendor lib handle not null at open time\n");
        return false;
    }

    mFdCmdOut = -1;
    mFdEvtIn = -1;
    mFdAclIn = -1;
    mFdAclOut = -1;

    mVendorLibHandle = dlopen(BT_VENDOR_LIB_NAME, 0);
    if (!mVendorLibHandle) {
        loge("Failed to open vendor lib\n");
        goto out;
    }

    mVendorIface = (const bt_vendor_interface_t*)dlsym(mVendorLibHandle, BT_VENDOR_INFACE_STRUCT);
    if (!mVendorIface) {

        loge("Failed to find vendor interface struct in vendor lib\n");
        goto out_close_lib;
    }

    if (mVendorIface->size < sizeof(bt_vendor_interface_t)) {
        loge("Vendor lib iface size %u is strange, expected %u\n",
            (int)mVendorIface->size, (int)sizeof(bt_vendor_interface_t));
        goto out_close_lib;
    }

    if (!sem_init(&mInitSem, 0, 0)) {
        loge("sem_init failed\n");
        goto out_close_lib;
    }

    if (!mVendorIface->init || !mVendorIface->op || !mVendorIface->cleanup) {
        loge("Missing essential vendor lib func(s)");
        goto out_close_lib;
    }

    return true;

out_close_lib:
    dlclose(mVendorLibHandle);

out:
    mVendorLibHandle = NULL;
    mVendorIface = NULL;
    return false;
}

/*
 * FUNCTION: vendorUp
 * USE:      Init the vendor library
 * PARAMS:   addr - the local bt addr (a copy is made)
 *           chipUpCbk - the callback for chip up status
 *           dataRxCbk - the callback for RX from the chip
 *           chipCbkData - data to pass to the clalbacks
 * RETURN:   success
 * NOTES:
 */
bool vendorUp(const uint8_t *addr, vendorCbkChipHwUp chipUpCbk, vendorCbkRx dataRxCbk, void *chipCbkData)
{
    int ret, fds[CH_MAX], pipeFds[2];
    static unsigned char local_addr[BT_MAC_LEN];
    static const bt_vendor_callbacks_t vendorCbks = {
        .size      = sizeof(bt_vendor_callbacks_t),
        .fwcfg_cb  = vcbkFwCfg,
        .scocfg_cb = vcbkScoCfg,
        .lpm_cb    = vcbkLbm,
        .alloc     = vcbkAlloc,
        .dealloc   = vcbkDealloc,
        .xmit_cb   = vcbkXmit,
        .epilog_cb = vcbkEpilog,
    };

    if (!chipUpCbk || !dataRxCbk) {
        loge("Missing callback for vendorUp()\n");
        return false;
    }

    if (!mVendorLibHandle) {
        loge("Vendor lib handle null at up time\n");
        return false;
    }

    mCbkRx = dataRxCbk;
    mCbkData = chipCbkData;

    memcpy(local_addr, addr, BT_MAC_LEN);

    ret = mVendorIface->init(&vendorCbks, local_addr);
    if (ret) {
        loge("iface init failed with code %d\n", ret);
        return false;
    }

    ret = mVendorIface->op(BT_VND_OP_POWER_CTRL, (void*)BT_VND_PWR_ON);
    if (ret) {
        loge("iface power on failed with code %d\n", ret);
        goto out_need_close;
    }

    chipUpCbk(chipCbkData, false, true);

    logi("Starting FW init\n");
    ret = mVendorIface->op(BT_VND_OP_FW_CFG, NULL);
    if (ret) {
        loge("iface fw config failed with code %d\n", ret);
        goto out_need_powerdown;
    }

    logi("Waiting for FW init\n");
    while(EAGAIN == sem_wait(&mInitSem));
    logi("FW init done: %s\n", mCbkResult ? "OK" : "FAILED");
    chipUpCbk(chipCbkData, true, mCbkResult);

    if (!mCbkResult)
        goto out_need_powerdown;

    ret = mVendorIface->op(BT_VND_OP_USERIAL_OPEN, &fds);
    if (ret == 1) {
        mFdCmdOut = fds[CH_CMD];
        mFdEvtIn = fds[CH_CMD];
    } else if (ret == 2) {
        mFdCmdOut = fds[CH_CMD];
        mFdEvtIn = fds[CH_CMD];
        mFdAclOut = fds[CH_ACL_OUT];
        mFdAclIn = fds[CH_ACL_OUT];
    } else if (ret == 4) {
        mFdCmdOut = fds[CH_CMD];
        mFdEvtIn = fds[CH_EVT];
        mFdAclOut = fds[CH_ACL_OUT];
        mFdAclIn = fds[CH_ACL_IN];
    } else {
        loge("iface userial open failed with code %d\n", ret);
        goto out_need_powerdown;
    }

    ret = pipe(pipeFds);
    if (ret) {
        loge("Failed to create vendor pipes\n");
        goto out_need_userial_close;
    }
    mWorkerClosePipe = pipeFds[1];

    ret = pthread_create(&mWorkerThread, NULL, vendorWorker, (void*)(uintptr_t)(pipeFds[0]));
    if (ret) {
        loge("Failed to create vendor worker thread\n");
        goto out_need_pipe_close;
    }


    return true;

out_need_pipe_close:
    close(pipeFds[0]);
    close(pipeFds[1]);

out_need_userial_close:
    mVendorIface->op(BT_VND_OP_USERIAL_CLOSE, NULL);

out_need_powerdown:
    mVendorIface->op(BT_VND_OP_EPILOG, NULL);
    logi("Waiting for epilog\n");
    while(EAGAIN == sem_wait(&mInitSem));
    logi("epilog done\n");
    mVendorIface->op(BT_VND_OP_POWER_CTRL, (void*)BT_VND_PWR_OFF);

out_need_close:
    mVendorIface->cleanup();
}

/*
 * FUNCTION: vendorTx
 * USE:      Send data to che chip
 * PARAMS:   typ - data type
 *           data - the data to send
 *           len - the length of the data
 * RETURN:   success
 * NOTES:    May block
 */
bool vendorTx(uint8_t typ, const void *data, uint32_t len)
{
    //todo
}


/*
 * FUNCTION: vendorDown
 * USE:      Bring down the chip
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    Shuts down the to-chip transport and the chip
 */
void vendorDown(void)
{
    int ret = 0;

    if (!mVendorLibHandle) {
        loge("Vendor lib handle null at down time\n");
        return;
    }

    ret = write(mWorkerClosePipe, &ret, 1);
    if (ret != 1)
        logw("Write to close vendor worker thread returned %d\n", ret);
    pthread_join(mWorkerThread, NULL);
    close(mWorkerClosePipe);

    mVendorIface->op(BT_VND_OP_USERIAL_CLOSE, NULL);
    mVendorIface->op(BT_VND_OP_EPILOG, NULL);
    logi("Waiting for epilog\n");
    while(EAGAIN == sem_wait(&mInitSem));
    logi("epilog done\n");
    mVendorIface->op(BT_VND_OP_POWER_CTRL, (void*)BT_VND_PWR_OFF);
    mVendorIface->cleanup();
}

/*
 * FUNCTION: vendorOpen
 * USE:      Close the vendor library
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    Only needs to be done once.
 */
void vendorClose(void)
{
    if (!mVendorLibHandle) {
        loge("Vendor lib handle null at close time\n");
        return;
    }

    dlclose(mVendorLibHandle);
    mVendorLibHandle = NULL;
    mVendorIface = NULL;
}


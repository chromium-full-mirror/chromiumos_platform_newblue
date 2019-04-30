/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <termios.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "bt_vendor_lib.h"
#include "vendorLib.h"
#include "config.h"
#include "timer.h"
#include "l2cap.h"
#include "util.h"
#include "uniq.h"
#include "hci.h"
#include "log.h"
#include "sg.h"
#include "mt.h"

#ifdef ANDROID
#define BT_VENDOR_LIB_NAME       "/vendor/lib/libbt-vendor.so"
#else
#define BT_VENDOR_LIB_NAME       "libbt-vendor.so"
#endif
#define BT_VENDOR_INFACE_STRUCT  "BLUETOOTH_VENDOR_LIB_INTERFACE"


static bool mLog = true;


/* our state w.r.t. vendor lib */
static void *mVendorLibHandle = NULL;
static const bt_vendor_interface_t *mVendorIface = NULL;
static vendorCbkRx mCbkRx;
static void* mCbkData;
static int mFdCmdOut;
static int mFdEvtIn;
static int mFdAclIn;
static int mFdAclOut;
static int mFdScoIn;
static int mFdScoOut;
static sem_t mInitSem;
static bool mCbkResult;
static sem_t mRxStartSem;
static pthread_t mWorkerThread;
static int mWorkerClosePipe;

static uint32_t mLpmTimeout;
static pthread_mutex_t mStateLock = PTHREAD_MUTEX_INITIALIZER;
static bool mKeepAwake;
static uniq_t mSleepTimer;
static bool mChipAwake;
static bool mNeedsCompleteWrites;
static bool mKnowAboutCompleWrites;
static bool mHaveLpm;
static bool mNeedTypeBytes;

static pthread_mutex_t mTxLock = PTHREAD_MUTEX_INITIALIZER;


/* for vendor lib callback */
static uint16_t mCmdCompleteWaiting = 0;
static tINT_CMD_CBACK mCmdCompleteCbk = NULL;

/* as per vendor lib docs */
typedef struct
{
    uint16_t          event;
    uint16_t          len;
    uint16_t          offset;
    uint16_t          layer_specific;
} HC_BT_HDR;


/*
 * FUNCTION: vendorSleepTimeout
 * USE:      Timer cbk for chip sleep
 * PARAMS:   timer - the timer
 *           userData - the uniqID of this request
 * RETURN:   NONE
 * NOTES:
 */
static void vendorSleepTimeout(uniq_t timer, uniq_t userData)
{
    uint8_t state = BT_VND_LPM_WAKE_DEASSERT;

    if (pthread_mutex_trylock(&mStateLock))
        timerSet(mLpmTimeout, vendorSleepTimeout, userData); /* reset for later then */
    else {
        if (mSleepTimer == userData && mChipAwake) {
            mChipAwake = false;
            /* letting chip sleep now */
            logd("LPM: going to bed\n");
            mVendorIface->op(BT_VND_OP_LPM_WAKE_SET_STATE, (void*)&state);
        }
        pthread_mutex_unlock(&mStateLock);
    }
}

/*
 * FUNCTION: vendorChipWake
 * USE:      Wake chip. if needed
 * PARAMS:   keepAwake - do not start auto-sleep timer
 * RETURN:   NONE
 * NOTES:
 */
static void vendorChipWake(bool keepAwake)
{
    uint8_t state = BT_VND_LPM_WAKE_ASSERT;

    if (!mLpmTimeout || !mHaveLpm)
        return;

    pthread_mutex_lock(&mStateLock);
    if (mSleepTimer)
        mSleepTimer = 0;

    if (!mChipAwake) {
        /* waking chip up now */
        logd("LPM: waking up\n");
        mVendorIface->op(BT_VND_OP_LPM_WAKE_SET_STATE, (void*)&state);
        mChipAwake = true;
    }

    if (!keepAwake && !mKeepAwake){
        mSleepTimer = uniqGetNext();
        timerSet(mLpmTimeout, vendorSleepTimeout, mSleepTimer);
        mKeepAwake = true;
    }

    pthread_mutex_unlock(&mStateLock);
}

/*
 * FUNCTION: vendorTxDoneWithEvts
 * USE:      Allow chip to sleep, if other causes allow
 * PARAMS:   tNONE
 * RETURN:   NONE
 * NOTES:
 */
void vendorTxDoneWithEvts(void)
{
    if (!mLpmTimeout || !mHaveLpm)
        return;

    pthread_mutex_lock(&mStateLock);
    mKeepAwake = false;
    mSleepTimer = uniqGetNext();
    timerSet(mLpmTimeout, vendorSleepTimeout, mSleepTimer);
    pthread_mutex_unlock(&mStateLock);
}


/*
 * FUNCTION: vcbkSomeConfigDone
 * USE:      Called when one of the types of configs is done
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void vcbkSomeConfigDone(bt_vendor_op_result_t result)
{
    logi("Some vendor config done\n");
    mCbkResult = result == BT_VND_OP_RESULT_SUCCESS;
    sem_post(&mInitSem);
}

/*
 * FUNCTION: vcbkAlloc
 * USE:      Alloc a buffer for vendor lib pass pointer to it past HC_BT_HDR, pre-fill hdr
 * PARAMS:   size
 * RETURN:   pointer
 * NOTES:    yes, this is weird. but the spec says so
 */
static void* vcbkAlloc(int size)
{
    HC_BT_HDR *h = (HC_BT_HDR*)malloc(sizeof(HC_BT_HDR) + size);
    if (!h) {
        loge("vendor alloc cbk fails for size %u\n", size);
        return NULL;
    }

    h->event = 0x2000;
    h->len = size;
    h->offset = 0;
    h->layer_specific = 0;

    return h;
}

/*
 * FUNCTION: vcbkDealloc
 * USE:      Free a buffer allocated by vcbkAlloc
 * PARAMS:   buffer
 * RETURN:   NONE
 * NOTES:    yes, this is weird. but the spec says so
 */
static void vcbkDealloc(void *p_buf)
{
    free(p_buf);
}

/*
 * FUNCTION: vcbkXmit
 * USE:      Send a command on behalf of the vendor lib
 * PARAMS:   opcode - the opcode sent (for matching reply)
 *           p_buf - the buffer containing the command
 *           p_cback - the callback to call when reply arrives
 * RETURN:   nonzero on success
 * NOTES:    We do nto implement it since the spec is unclear and nobody uses this
 */
static uint8_t vcbkXmit(uint16_t opcode, void *p_buf, tINT_CMD_CBACK p_cback)
{
    HC_BT_HDR *h = (HC_BT_HDR*)p_buf;
    sg cmdSg;
    uint8_t *cmd = (uint8_t*)(h + 1);
    uint8_t paramLen = cmd[2]; /* as per vendor code */
    uint16_t totalLen = 3 + paramLen;

    mCmdCompleteWaiting = opcode;
    mCmdCompleteCbk = p_cback;

    /* we hand the pointer to sg - it will free it */
    cmdSg = sgNewWithAllocedData(p_buf, totalLen + sizeof(HC_BT_HDR));
    if (!cmdSg) {
        loge("Failed to alloc sg for vendor lib tx\n");
        return 0;
    }
    /* but we also do not want to send the header, so we cleverly truncate it away */
    sgTruncFront(cmdSg, sizeof(HC_BT_HDR));

    if (vendorTx(HCI_PKT_TYP_CMD, cmdSg, false))
        return 1;

    loge("vendorTx() failed for vendor lib cmd\n");
    sgFree(cmdSg);
    return 0;
}

/*we are ready to RX*/
/*
 * FUNCTION: vendorStartRx
 * USE:      Start vendor lib listening to the chip. No RX events will be delivered
 *           before this is called
 * PARAMS:   none
 * RETURN:   none
 * NOTES:
 */
void vendorStartRx(void)
{
    sem_post(&mRxStartSem);
}

/*
 * FUNCTION: vendorWorker
 * USE:      Vendor lib worker thread
 * PARAMS:   pipe - actually an int - the fd with the pipe used to tell us to quit
 * RETURN:   unused
 * NOTES:
 */
static void* vendorWorker(void *pipeFdP)
{
    int pipeFd = (uintptr_t)pipeFdP;
    int i, ret;

    pthread_setname_np(pthread_self(), "bt_vendor_worker");

    r_sem_wait(&mRxStartSem);

    while (1) {
        struct pollfd pollfds[CH_MAX + 1] = {{0,},};
        int numfds = 0, exitPipeIdx;

        /* add our pipe for signaling a shutdown */
        pollfds[numfds].fd = pipeFd;
        pollfds[numfds].events = POLLIN | POLLPRI | POLLERR;
        exitPipeIdx = numfds++;

        /* add others */
        if (mFdAclIn != -1) {
            pollfds[numfds].fd = mFdAclIn;
            pollfds[numfds].events = POLLIN;
            numfds++;
        }
        if (mFdScoIn != mFdAclIn) {
            pollfds[numfds].fd = mFdScoIn;
            pollfds[numfds].events = POLLIN;
            numfds++;
        }
        if (mFdEvtIn != mFdAclIn) {
            pollfds[numfds].fd = mFdEvtIn;
            pollfds[numfds].events = POLLIN;
            numfds++;
        }

        /* wait for action */
        while((ret = poll(pollfds, numfds, -1)) < 0 && errno == EINTR);
        if (ret < 0) {
            loge("Unexpected poll error %d\n", ret);
            continue;
        } else if (!ret) {
            loge("Unexpected poll timeout\n");
            continue;
        }

        if (pollfds[exitPipeIdx].revents) {
            logi("Vendor worker shutdown with events %u\n", pollfds[exitPipeIdx].revents);
            break;
        }

        for(i = 0; i < numfds; i++) {
            uint8_t buf[5], hdrSz = 0;
            uint32_t sz, packetOffset = 0;
            uint8_t *packet;

            if (i == exitPipeIdx)
                continue;

            if (!pollfds[i].revents)
                continue;

            if (!mNeedTypeBytes) {
                if (pollfds[i].fd == mFdEvtIn)
                    buf[0] = HCI_PKT_TYP_EVT;
                else if (pollfds[i].fd == mFdAclIn)
                    buf[0] = HCI_PKT_TYP_ACL;
                else if (pollfds[i].fd == mFdScoIn)
                    buf[0] = HCI_PKT_TYP_SCO;
                else
                    loge("Unknown FD\n");
            }

            /* read the packet */
            if (!mNeedsCompleteWrites && (!mNeedTypeBytes || r_read(pollfds[i].fd, buf, 1))){

                switch (buf[0]) {
                case HCI_PKT_TYP_ACL:
                    if (!r_read(pollfds[i].fd, buf + 1, hdrSz = 4)) {
                        loge("Failed to read ACL hdr: %d\n", errno);
                        continue;
                    }
                    sz = utilGetLE16(buf + 3);
                    break;
                case HCI_PKT_TYP_SCO:
                    if (!r_read(pollfds[i].fd, buf + 1, hdrSz = 3)) {
                        loge("Failed to read SCO hdr: %d\n", errno);
                        continue;
                    }
                    sz = utilGetLE8(buf + 3);
                    break;
                case HCI_PKT_TYP_EVT:
                    if (!r_read(pollfds[i].fd, buf + 1, hdrSz = 2)) {
                        loge("Failed to read EVT hdr: %d\n", errno);
                        continue;
                    }
                    sz = utilGetLE8(buf + 2);
                    break;
                default:
                    loge("Unknown HCI packet type %d\n", buf[0]);
                    continue;
                }
                packet = (uint8_t*)malloc(sz + hdrSz);
                if (!packet) {
                    loge("Failed to alloc %u bytes for packet\n", sz + hdrSz);
                    /* read it anyways to avoid desync */
                    while(sz--)
                        r_read(pollfds[i].fd, buf, 1);
                    continue;
                }
                memcpy(packet, buf + 1, hdrSz);
                sz += hdrSz;
                if (!r_read(pollfds[i].fd, packet + hdrSz, sz - hdrSz)) {
                    loge("Failed to read packet remainder of a %ub packet\n", sz);
                    free(packet);
                    continue;
                }
            } else if (mNeedsCompleteWrites || errno == ENOMEM) { /* maybe they want us to read the entire packet at once */
                uint32_t readBufOffst = mNeedTypeBytes ? 0 : 1;

                int bytesAvail = -1;
                if (ioctl(pollfds[i].fd, FIONREAD, &bytesAvail)) {
                    loge("ioctl.FIONREAD failed too: %d\n", errno);
                    continue;
                }
                if (bytesAvail <= 0) {
                    loge("ioctl.FIONREAD told us there are %u bytes\n", bytesAvail);
                    continue;
                }
                packet = (uint8_t*)malloc(bytesAvail + readBufOffst);
                if (!packet) {
                    loge("alloc(%u) fails\n", bytesAvail);
                    continue;
                }

                if (!r_read(pollfds[i].fd, packet + readBufOffst, bytesAvail)) {
                    free(packet);
                    loge("second read fails: %d\n", errno);
                    continue;
                }

                if (!mNeedTypeBytes)
                    packet[0] = buf[0];

                switch (packet[0]) {
                case HCI_PKT_TYP_ACL:
                    hdrSz = 4;
                    sz = utilGetLE16(packet + 3);
                    break;
                case HCI_PKT_TYP_SCO:
                    hdrSz = 3;
                    sz = utilGetLE8(packet + 3);
                    break;
                case HCI_PKT_TYP_EVT:
                    hdrSz = 2;
                    sz = utilGetLE8(packet + 2);
                    break;
                default:
                    loge("Unknown HCI packet type %d\n", packet[0]);
                    free(packet);
                    continue;
                }
                sz += hdrSz;
                packetOffset = 1;
                if ((unsigned)bytesAvail != sz + packetOffset) {
                    loge("%u (incl %u) + %u != %u\n", sz, hdrSz, packetOffset, bytesAvail);
                    free(packet);
                    continue;
                }
                mNeedsCompleteWrites = true;
                buf[0] = packet[0];
            } else {
                loge("Failed to read packet type: %d\n", errno);
                continue;
            }

            mKnowAboutCompleWrites = true;
            if (mLog) {
                char line[128];
                uint32_t i;

            if (!sz)
                sprintf(line, "vRX (%u) <ZLP>", buf[0]);

                for(i = 0; i < sz; i++) {
                    if (i & 15)
                        sprintf(line + strlen(line), " %02X", packet[i + packetOffset]);
                    else {
                        if (i)
                            logv("%s\n", line);
                        sprintf(line, "vRX (%d) [%03X] %02X", buf[0], i, packet[i + packetOffset]);
                    }
                }
                logv("%s\n", line);
            }

            /* vendor lib code has first dibs on valid command complete packets */
            if (mCmdCompleteWaiting && buf[0] == HCI_PKT_TYP_EVT && packet[0 + packetOffset] == 0x0E && sz >= 5) {

                uint16_t code = (((uint16_t)packet[4 + packetOffset]) << 8) | packet[3 + packetOffset];
                /* but it only needs them of a certain type - so we filter here */
                if (code == mCmdCompleteWaiting) {
                    mCmdCompleteWaiting = 0;


                    HC_BT_HDR *h = (HC_BT_HDR*)vcbkAlloc(sz);
                    if (!h)
                        loge("Failed to alloc reply for vendor complete event\n");
                    else {
                        memcpy(h + 1, packet + packetOffset, sz);
                        mCmdCompleteCbk(h);
                    }
                    free(packet);
                    packet = NULL;
                }
            }
            if (packet) {
                sg data = sgNewWithAllocedData(packet, sz + packetOffset);
                if (data) {
                    sgTruncFront(data, packetOffset);
                    mCbkRx(mCbkData, buf[0], data);
                } else {
                    loge("failed to alloc RX sg\n");
                    free(packet);
                }
            }
        }
    }

    /* clean up on exit */
    close(pipeFd);
    return NULL;
}

/*
 * FUNCTION: vendorLogEnable
 * USE:      Enable/disable logging
 * PARAMS:   on - enable logging?
 * RETURN:   NONE
 * NOTES:
 */
void vendorLogEnable(bool on)
{
    mLog = on;
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
    mFdScoIn = -1;
    mFdScoOut = -1;
    mCmdCompleteWaiting = 0;
    mKeepAwake = true;
    mChipAwake = false;
    mLpmTimeout = 0;
    mSleepTimer = 0;


    mVendorLibHandle = dlopen(BT_VENDOR_LIB_NAME, RTLD_NOW);
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
        /* we cannot fail out of here since TI actually does this! */
        //goto out_close_lib;
    }

    if (sem_init(&mInitSem, 0, 0)) {
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
bool vendorUp(const uint8_t *addr, vendorCbkChipUp chipUpCbk, vendorCbkRx dataRxCbk, void *chipCbkData)
{
    uint8_t tv8;
    int ret, tmp, fds[CH_MAX], pipeFds[2], freeFds[CH_MAX], nfds, *fdsP = fds;
    static unsigned char local_addr[BT_MAC_LEN];
    static const bt_vendor_callbacks_t vendorCbks = {
        .size      = sizeof(bt_vendor_callbacks_t),
        .fwcfg_cb  = vcbkSomeConfigDone,
        .scocfg_cb = vcbkSomeConfigDone,
        .lpm_cb    = vcbkSomeConfigDone,
        .alloc     = vcbkAlloc,
        .dealloc   = vcbkDealloc,
        .xmit_cb   = vcbkXmit,
        .epilog_cb = vcbkSomeConfigDone,
    };

    mHaveLpm = true;

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

    logd("vendor->init\n");
    ret = mVendorIface->init(&vendorCbks, local_addr);
    if (ret) {
        loge("iface init failed with code %d\n", ret);
        return false;
    }

    logd("vendor->op(BT_VND_OP_POWER_CTRL, off)\n");
    tmp = BT_VND_PWR_OFF;
    ret = mVendorIface->op(BT_VND_OP_POWER_CTRL, (void*)&tmp);
    if (ret) {
        loge("iface power off failed with code %d\n", ret);
        /* we cannot goto out_need_close here since QC chips will then fail */
    }

    if (mHaveLpm) {
        logd("vendor->op(BT_VND_OP_GET_LPM_IDLE_TIMEOUT)\n");
        ret = mVendorIface->op(BT_VND_OP_GET_LPM_IDLE_TIMEOUT, (void*)&mLpmTimeout);
        if (ret) {
            loge("iface LPM TO get failed with code %d\n", ret);
            mHaveLpm = false;
        }
        logi("Vendor timeout: %ums\n", (unsigned)mLpmTimeout);
    }

    logd("vendor->op(BT_VND_OP_POWER_CTRL, on)\n");
    tmp = BT_VND_PWR_ON;
    ret = mVendorIface->op(BT_VND_OP_POWER_CTRL, (void*)&tmp);
    if (ret) {
        loge("iface power on failed with code %d\n", ret);
        goto out_need_close;
    }

    chipUpCbk(chipCbkData, false, true);

    logd("vendor->op(BT_VND_OP_USERIAL_OPEN)\n");
    nfds = mVendorIface->op(BT_VND_OP_USERIAL_OPEN, &fdsP);
    logd("nfds %d\n", nfds);
    mNeedTypeBytes = false;
    if (nfds == 1) {
        mFdCmdOut = fds[CH_CMD];
        mFdEvtIn = fds[CH_CMD];
        mFdAclIn = fds[CH_CMD];
        mFdAclOut = fds[CH_CMD];
        mFdScoIn = fds[CH_CMD];
        mFdScoOut = fds[CH_CMD];
        freeFds[0] = fds[CH_CMD];
        mNeedTypeBytes = true;
    } else if (nfds == 2) {
        mFdCmdOut = fds[CH_CMD];
        mFdEvtIn = fds[CH_CMD];
        mFdAclOut = fds[CH_ACL_OUT];
        mFdAclIn = fds[CH_ACL_OUT];
        mFdScoIn = -1; /* SCO cannot be supported in this configuration */
        mFdScoOut = -1;
        freeFds[0] = fds[CH_CMD];
        freeFds[1] = fds[CH_ACL_OUT];
    } else if (nfds == 4) {
        mFdCmdOut = fds[CH_CMD];
        mFdEvtIn = fds[CH_EVT];
        mFdAclOut = fds[CH_ACL_OUT];
        mFdAclIn = fds[CH_ACL_IN];
        mFdScoIn = -1; /* SCO cannot be supported in this configuration */
        mFdScoOut = -1;
        freeFds[0] = fds[CH_CMD];
        freeFds[1] = fds[CH_EVT];
        freeFds[2] = fds[CH_ACL_IN];
        freeFds[3] = fds[CH_ACL_OUT];
    } else {
        loge("iface userial open failed with code %d\n", nfds);
        goto out_need_powerdown;
    }

    ret = pipe(pipeFds);
    if (ret) {
        loge("Failed to create vendor pipes\n");
        goto out_need_userial_close;
    }
    mWorkerClosePipe = pipeFds[1];

    if (sem_init(&mRxStartSem, 0, 0)) {
        loge("sem_init failed\n");
        goto out_need_pipe_close;
    }
    ret = pthread_create(&mWorkerThread, NULL, vendorWorker, (void*)(uintptr_t)(pipeFds[0]));
    if (ret) {
        loge("Failed to create vendor worker thread\n");
        goto out_need_sem_deinit;
    }

    logd("Starting FW init\n");
    ret = mVendorIface->op(BT_VND_OP_FW_CFG, NULL);
    if (ret) {
        loge("iface fw config failed with code %d\n", ret);
        goto out_need_thread_shutdown;
    }

    logd("Waiting for FW init\n");
    r_sem_wait(&mInitSem);
    logd("FW init done: %s\n", mCbkResult ? "OK" : "FAILED");
    chipUpCbk(chipCbkData, true, mCbkResult);
    if (!mCbkResult)
        goto out_need_thread_shutdown;

    if (mHaveLpm) {
        logd("Setting LPM mode\n");
        tv8 = BT_VND_LPM_ENABLE;
        ret = mVendorIface->op(BT_VND_OP_LPM_SET_MODE, (void*)&tv8);
        if (!ret) { /* yup .. this one returns nonzero on error */
            loge("iface LPM config failed with code %d\n", ret);
            mHaveLpm = false;
        }
        if (mHaveLpm) {
            logd("Waiting for LPM init\n");
            r_sem_wait(&mInitSem);
            logd("LPM init done: %s\n", mCbkResult ? "OK" : "FAILED");
        }
    }

    return true;

out_need_thread_shutdown:
    mVendorIface->op(BT_VND_OP_EPILOG, NULL);
    logi("Waiting for epilog\n");
    r_sem_wait(&mInitSem);
    logi("epilog done\n");

    tmp = 0;
    ret = write(mWorkerClosePipe, &tmp, 1);
    if (ret != 1)
        logw("Write to close vendor worker thread returned %d\n", ret);
    pthread_join(mWorkerThread, NULL);

out_need_sem_deinit:
    sem_destroy(&mRxStartSem);

out_need_pipe_close:
    close(pipeFds[0]);
    close(pipeFds[1]);

out_need_userial_close:
    while (nfds)
        close(freeFds[--nfds]);
    mVendorIface->op(BT_VND_OP_USERIAL_CLOSE, NULL);

out_need_powerdown:
    tmp = BT_VND_PWR_ON;
    mVendorIface->op(BT_VND_OP_POWER_CTRL, (void*)&tmp);

out_need_close:
    mVendorIface->cleanup();

    return false;
}

/*
 * FUNCTION: vendorTx
 * USE:      Send data to che chip
 * PARAMS:   typ - data type
 *           data - the data to send
 *           expectReply - keep chip awake?
 * RETURN:   success
 * NOTES:    May block
 */
bool vendorTx(uint8_t typ, sg data, bool expectReply)
{
    bool ret = false;
    void *iter;
    int fd;

    pthread_mutex_lock(&mTxLock);
    vendorChipWake(false);

    if (mLog) {
        char line[128] = "vTX <ZLP>";
        uint32_t i;
        uint8_t v;

        for(i = 0; i < sgLength(data); i++) {
            sgSerialize(data, i, 1, &v);
            if (i & 15)
                sprintf(line + strlen(line), " %02X", v);
            else {
                if (i)
                    logv("%s\n", line);
                sprintf(line, "vTX (%d) [%03X] %02X", typ, i, v);
            }
        }
        logv("%s\n", line);
    }

    if (mNeedTypeBytes && !sgConcatFrontCopy(data, &typ, 1)) {
        loge("TX type concat fail\n");
        goto out;
    }

    switch (typ) {
    case HCI_PKT_TYP_CMD:
        fd = mFdCmdOut;
        break;
    case HCI_PKT_TYP_ACL:
        fd = mFdAclOut;
        break;
    case HCI_PKT_TYP_SCO:
        fd = mFdAclOut; /* may be a reasonable guess */
        break;
    default:
        loge("Unknown packet send type: %d\n", typ);
        goto out;
    }

    if (mNeedsCompleteWrites || !mKnowAboutCompleWrites) {
        static uint8_t *txBufPtr = NULL;
        static uint32_t txBufSz = 0;
        uint8_t *tmp;

        if (txBufSz < sgLength(data)) {
            tmp = (uint8_t*)realloc(txBufPtr, sgLength(data));
            if (!tmp) {
                loge("Failed to realloc TXB to %ub\n", sgLength(data));
                goto out;
            }
            txBufPtr = tmp;
            txBufSz = sgLength(data);
        }
        if (sgSerialize(data, 0, sgLength(data), txBufPtr) != sgLength(data)) {
            loge("Failed to serialize to TXB\n");
            goto out;
        }
        if (!r_write(fd, txBufPtr, sgLength(data))) {
            loge("Write failed for data %d: %d\n", fd, errno);
            goto out;
        }
    } else {
        /* quite elegant, don't you find? */
        for (iter = sgIterStart(data); iter; iter = sgIterAdvance(iter)) {
            if (!r_write(fd, sgIterCurData(iter), sgIterCurLen(iter))) {
                loge("Write failed for data %d: %d\n", fd, errno);
                goto out;
            }
        }
    }

    sgFree(data);
    ret = true;

out:
    vendorChipWake(expectReply);
    pthread_mutex_unlock(&mTxLock);
    return ret;
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
    int ret = 0, tmp;

    if (!mVendorLibHandle) {
        loge("Vendor lib handle null at down time\n");
        return;
    }

    logd("vendor->op(BT_VND_OP_EPILOG)\n");
    mVendorIface->op(BT_VND_OP_EPILOG, NULL);
    logi("Waiting for epilog\n");
    r_sem_wait(&mInitSem);
    logi("epilog done\n");
    close(mFdCmdOut);
    if (mFdEvtIn != mFdCmdOut)
        close(mFdEvtIn);
    if (mFdAclIn != mFdCmdOut)
        close(mFdAclIn);
    if (mFdAclOut != mFdAclIn)
        close(mFdAclOut);
    if (mFdScoIn != mFdAclIn)
        close(mFdScoIn);
    if (mFdScoOut != mFdScoIn)
        close(mFdScoOut);
    mVendorIface->op(BT_VND_OP_USERIAL_CLOSE, NULL);

    ret = write(mWorkerClosePipe, &ret, 1);
    if (ret != 1)
        logw("Write to close vendor worker thread returned %d\n", ret);
    logd("Waiting for vendor worker to finish\n");
    sem_post(&mRxStartSem);
    pthread_join(mWorkerThread, NULL);
    sem_destroy(&mRxStartSem);
    logd("Vendor worker done\n");
    close(mWorkerClosePipe);

    logd("vendor->op(BT_VND_OP_POWER_CTRL, off)\n");
    tmp = BT_VND_PWR_OFF;
    mVendorIface->op(BT_VND_OP_POWER_CTRL, (void*)&tmp);
    logd("vendor->cleanup()\n");
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


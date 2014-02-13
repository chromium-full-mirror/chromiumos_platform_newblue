//todo: BT_VND_OP_SCO_CFG
//todo: BT_VND_OP_GET_LPM_IDLE_TIMEOUT
//todo: BT_VND_OP_LPM_SET_MODE
//todo: BT_VND_OP_LPM_WAKE_SET_STATE

#include <semaphore.h>
#include <pthread.h>
#include <stdlib.h>
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


static bool mLog = false;


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

    if (vendorTx(HCI_PKT_TYP_CMD, cmdSg))
        return 1;

    loge("vendorTx() failed for vendor lib cmd\n");
    sgFree(cmdSg);
    return 0;
}

/*
 * FUNCTION: rread
 * USE:      Read while handing EINTR
 * PARAMS:   fd - the fd
 *           buf - where to read to
 *           sz - the size
 * RETURN:   0 is all ok, else error
 * NOTES:
 */
static int rread(int fd, void *buf, uint32_t sz)
{
    uint8_t *dst = (uint8_t*)buf;
    int ret;

    while (sz) {

        ret = read(fd, dst, sz);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return errno ? errno : -1;
        } else if (!ret)
            logw("Read returned 0 unexpectedly\n");
        dst += ret;
        sz -= ret;
    }

    return 0;
}

/*
 * FUNCTION: rwrite
 * USE:      Write while handing EINTR
 * PARAMS:   fd - the fd
 *           buf - where to write from
 *           sz - the size
 * RETURN:   0 is all ok, else error
 * NOTES:
 */
static int rwrite(int fd, const void *buf, uint32_t sz)
{
    const uint8_t *dst = (const uint8_t*)buf;
    int ret;

    while (sz) {

        ret = write(fd, dst, sz);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return errno ? errno : -1;
        } else if (!ret)
            logw("Write returned 0 unexpectedly\n");
        dst += ret;
        sz -= ret;
    }

    return 0;
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

    while (1) {
        struct pollfd pollfds[CH_MAX + 1] = {{0,},};
        int numfds = 0, exitPipeIdx;
        int firstFd = -1;
        int secondFd = -1;

        /* add our pipe for signaling a shutdown */
        pollfds[numfds].fd = pipeFd;
        pollfds[numfds].events = POLLIN | POLLPRI | POLLERR;
        exitPipeIdx = numfds++;

        /* add others */
        if (mFdAclIn) {
            pollfds[numfds].fd = mFdAclIn;
            pollfds[numfds].events = POLLIN;
            numfds++;
        }
        if (mFdEvtIn != mFdAclIn) {
            pollfds[numfds].fd = mFdEvtIn;
            pollfds[numfds].events = POLLIN;
            numfds++;
        }

        /* wait for action */
        while(EINTR == (ret = poll(pollfds, numfds, -1)));
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
            uint8_t buf[5], extraSz = 0, hdrSz = 0;
            uint32_t sz;
            uint8_t *packet;

            if (i == exitPipeIdx)
                continue;

            if (!pollfds[i].revents)
                continue;

            /* read the packet */
            ret = rread(pollfds[i].fd, buf, 1);
            if (ret) {
                loge("Failed to read packet type: %d\n", ret);
                continue;
            }
            switch (buf[0]) {
            case HCI_PKT_TYP_ACL:
                ret = rread(pollfds[i].fd, buf + 1, hdrSz = 4);
                if (ret) {
                    loge("Failed to read ACL hdr: %d\n", ret);
                    continue;
                }
                sz = utilGetLE16(buf + 3);
                break;
            case HCI_PKT_TYP_SCO:
                ret = rread(pollfds[i].fd, buf + 1, hdrSz = 3);
                if (ret) {
                    loge("Failed to read SCO hdr: %d\n", ret);
                    continue;
                }
                sz = utilGetLE8(buf + 3);
                break;
            case HCI_PKT_TYP_EVT:
                ret = rread(pollfds[i].fd, buf + 1, hdrSz = 2);
                if (ret) {
                    loge("Failed to read EVT hdr: %d\n", ret);
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
                    rread(pollfds[i].fd, buf, 1);
                continue;
            }
            memcpy(packet, buf + 1, hdrSz);
            sz += hdrSz;
            ret = rread(pollfds[i].fd, packet + hdrSz, sz - hdrSz);
            if (ret) {
                loge("Failed to read packet remainder of a %ub packet\n", sz);
                free(packet);
                continue;
            }

            if (mLog) {
                char x[128];
                unsigned i;
                sprintf(x,"RX (%4u): %02X", sz, buf[0]);
                for(i = 0; i < 32 && i < sz; i++) sprintf(x + strlen(x), " %02X", ((uint8_t*)packet)[i]);
                if (i < sz) strcat(x, " ...");
                logd("%s\n", x);
            }

            /* vendor lib code has first dibs on valid command complete packets */
            if (mCmdCompleteWaiting && buf[0] == HCI_PKT_TYP_EVT && packet[0] == 0x0E && sz >= 5) {

                uint16_t code = (((uint16_t)packet[4]) << 8) | packet[3];

                /* but it only needs them of a certain type - so we filter here */
                if (code == mCmdCompleteWaiting) {
                    mCmdCompleteWaiting = 0;


                    HC_BT_HDR *h = (HC_BT_HDR*)vcbkAlloc(sz);
                    if (!h)
                        loge("Failed to alloc reply for vendor complete event\n");
                    else {
                        memcpy(h + 1, packet, sz);
                        mCmdCompleteCbk(h);
                    }
                    free(packet);
                    packet = NULL;
                }
            }
            if (packet) {
                mCbkRx(mCbkData, buf[0], packet, sz);
                free(packet);
            }
        }
    }

    /* clean up on exit */
    close(pipeFd);
    return NULL;
}

/*
 * FUNCTION: vendorOpen
 * USE:      Open the vendor library
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:    Only needs to be done once.
 *           Keep it open from then on.
 */
bool vendorOpen(bool logAll)
{
    if (mVendorLibHandle) {
        loge("Vendor lib handle not null at open time\n");
        return false;
    }

    mFdCmdOut = -1;
    mFdEvtIn = -1;
    mFdAclIn = -1;
    mFdAclOut = -1;
    mCmdCompleteWaiting = 0;

    mLog = logAll;

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
    int ret, tmp, fds[CH_MAX], pipeFds[2];
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

    logd("vendor->op(BT_VND_OP_POWER_CTRL, on)\n");
    tmp = BT_VND_PWR_ON;
    ret = mVendorIface->op(BT_VND_OP_POWER_CTRL, (void*)&tmp);
    if (ret) {
        loge("iface power on failed with code %d\n", ret);
        goto out_need_close;
    }

    chipUpCbk(chipCbkData, false, true);

    logd("vendor->op(BT_VND_OP_USERIAL_OPEN)\n");
    ret = mVendorIface->op(BT_VND_OP_USERIAL_OPEN, &fds);
    logd("nfds %d\n", ret);
    if (ret == 1) {
        mFdCmdOut = fds[CH_CMD];
        mFdEvtIn = fds[CH_CMD];
        mFdAclIn = fds[CH_CMD];
        mFdAclOut = fds[CH_CMD];
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

out_need_pipe_close:
    close(pipeFds[0]);
    close(pipeFds[1]);

out_need_userial_close:
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
 * RETURN:   success
 * NOTES:    May block
 */
bool vendorTx(uint8_t typ, sg data)
{
    int fd, ret = 0;
    void *iter;

    if (!sgConcatFrontCopy(data, &typ, 1))
        return false;

    if (mLog) {
       char x[128];
       unsigned i;
       sprintf(x,"TX (%4u):", sgLength(data));
       for(i = 0; i < 33 && i < sgLength(data); i++) { /* this is not even remotely a good idea, speedwise */
           uint8_t byte;
           sgSerialize(data, i, 1, &byte);
           sprintf(x + strlen(x), " %02X", byte);
       }
       if (i < sgLength(data)) strcat(x, " ...");
       logd("%s\n", x);
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
        return false;
    }

    /* quite elegant, don't you find? */
    for (iter = sgIterStart(data); iter; iter = sgIterAdvance(iter)) {
        ret = rwrite(fd, sgIterCurData(iter), sgIterCurLen(iter));
        if (ret) {
            loge("Write failed for data %d: %d\n", fd, ret);
            return false;
        }
    }

    sgFree(data);
    return true;   
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
    mVendorIface->op(BT_VND_OP_USERIAL_CLOSE, NULL);

    ret = write(mWorkerClosePipe, &ret, 1);
    if (ret != 1)
        logw("Write to close vendor worker thread returned %d\n", ret);
    logd("Waiting for vendor worker to finish\n");
    pthread_join(mWorkerThread, NULL);
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


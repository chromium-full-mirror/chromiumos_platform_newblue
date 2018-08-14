/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <regex.h>

#include "newblue-macros.h"
#include "gattSvcMiscInfo.h"
#include "gattSvcBattery.h"
#include "gatt-builtin.h"
#include "vendorLib.h"
#include "btleHid.h"
#include "uhid.h"
#include "l2cap.h"
#include "timer.h"
#include "aapi.h"
#include "util.h"
#include "uuid.h"
#include "gatt.h"
#include "att.h"
#include "hci.h"
#include "log.h"
#include "bt.h"
#include "mt.h"

struct LeDiscDev {
    struct LeDiscDev  *next;
    uint8_t            mac[BT_MAC_LEN];
    bool               randomAddr;
    bool               haveRsp;
    int8_t             rssi;
    uint8_t            advType;
    uint8_t            advLen;
    uint8_t            rspLen;
    uint8_t            advData[31];
    uint8_t            rspData[31];
};

struct EdrDiscDev {
    struct EdrDiscDev *next;
    uint8_t            mac[BT_MAC_LEN];
    uint32_t           devCls;
    int8_t             rssi;
    bool               haveEir;
    bool               haveName;
    uint8_t            eir[240];
    char               name[249];
};

struct ServiceList {
    struct ServiceList* next;
    uint16_t start, end;
    struct uuid uuid;
};

static sem_t mGattTestSem;
static struct ServiceList *mGattSvcs = NULL;
static ble_hid_conn_t mHidConn = 0;
static gatt_svc_batt_conn_t mBattSvcCon = 0;
static gatt_svc_misc_info_conn_t mMiscInfoSvcCon = 0;

static const char* getAppearanceTypeStr(uint32_t appVal);
static const char* getAppearanceSubtypeStr(uint32_t appVal);


static void testHciScoRxCbk(hci_conn_t aclConn, const void *data, uint8_t len, uint8_t lossAmount)
{
   static const char *lossAmtStr[] = {"none", "some", "unknown", "all"};

   logi("TEST SCO RX %ub, loss: %s\n", len, lossAmtStr[lossAmount]);
}

static void leDiscoveryCbk(void *cbkData, const struct bt_addr *addr, int8_t rssi, uint8_t replyType, const void *eir, uint8_t eirLen)
{
    struct LeDiscDev **leDevicesP = (struct LeDiscDev**)cbkData, *t;
    char randomAddr = addr->type == BT_ADDR_TYPE_LE_RANDOM;

    /* see if this mac exists */
    t = *leDevicesP;
    while (t && (t->randomAddr != randomAddr || memcmp(t->mac, addr->addr, sizeof(t->mac))))
        t = t->next;

    if (!t) {// not found

        t = (struct LeDiscDev*)calloc(1, sizeof(struct LeDiscDev));
        if (!t)
            return;

        t->randomAddr = randomAddr;
        memmove(t->mac, addr->addr, BT_MAC_LEN);
        t->next = *leDevicesP;
        *leDevicesP = t;
        fprintf(stderr, ".");
    }

    t->rssi = rssi;
    if (replyType != HCI_ADV_TYPE_SCAN_RSP) {
        t->advType = replyType;
        memcpy(t->advData, eir, eirLen);
        t->advLen = eirLen;
    } else {
        if (!t->haveRsp)
            fprintf(stderr, ",");
        t->haveRsp = true;
        memcpy(t->rspData, eir, eirLen);
        t->rspLen = eirLen;
    }
}

static void readyForUpCbk() {
  logi("callback stack is ready for up\n");
}

#define DEVICE_CLASS_SERVICE_RENDERING		0x040000	//PRINTER, SPEAKER
#define DEVICE_CLASS_SERVICE_AUDIO		0x200000	//SPEAKER, MIC, HEADSET
#define DEVICE_CLASS_SERVICE_INFORMATION	0x800000	//WEB-server, WAP-server

#define DEVICE_CLASS_MAJOR_SHIFT		8

#define DEVICE_CLASS_MAJOR_AV			4

#define DEVICE_CLASS_MINOR_AV_SHIFT		2
#define DEVICE_CLASS_MINOR_AV_PORTBL_AUDIO	7

void logData(char* dst, const void *buf, uint8_t len)
{
    const uint8_t *data = (const uint8_t*)buf;
    unsigned i;

    sprintf(dst, "(%ub):", len);
    for (i = 0; i < len; i++)
        sprintf(dst + strlen(dst), " %02X", data[i]);
}

static gatt_service_t mSvcTime = 0;
static att_cid_t mConnTime = 0;
static bool stopTimeService = false;

uint16_t mGattHandleCurTime = 0;
uint16_t mGattHandleCurTimeCfg = 0;
uint16_t mGattHandleLocalTimeInfo = 0;
uint16_t mGattHandleReferenceTimeInfo = 0;

static uniq_t terminateTimerId = 0;
static bool terminate = false;

static hci_adv_set_t mAdvSet[3] = {0,};

static struct bt_addr mAddrs[3];
static struct bt_addr mInputDevAddr;
uniq_t mPairStateObserverId = 0;
uniq_t mPasskeyDisplayObserverId = 0;
uniq_t mPasskeyRequestObserverId = 0;

struct dateTime {
    uint16_t year;    // used in LE service with little endian.
    uint8_t month;
    uint8_t day;
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;
} __packed;

struct dayOfWeek {
    uint8_t dayOfWeek;
} __packed;

struct dayDateTime {
    struct dateTime dateTime;
    struct dayOfWeek dayOfWeek;
} __packed;

struct exactTime256 {
    struct dayDateTime dayDateTime;
    uint8_t fractions256;
} __packed;

struct {
    struct exactTime256 exactTime256;
    uint8_t adjustReason;
}__packed mGattCurTime;

struct {
    uint16_t cCC; // Client Characteristic Configuration descriptor
}__packed mGattCurTimeCfg;

struct {
    int8_t timeZone;    // offset from UTC in 15-min increments
    uint8_t dstOffset;  // daylight saving time offset to add
}__packed mGattLocalTimeInfo;

bool isSameAddr(const struct bt_addr *a, const struct bt_addr *b) {
    if (!a || !b)
        return false;
    return !memcmp(a, b, sizeof(struct bt_addr));
}

static bool testGattSrvValueReadCbk(gatt_service_t svc, l2c_handle_t who, att_cid_t cid, att_trans_t transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t wantedLen)
{
    uint8_t err = ATT_ERROR_NONE, i;
    const uint8_t *ptr = NULL;
    uint16_t len = 0;
    struct bt_addr srvAddr;

    if (svc != mSvcTime) {
        logw("Unexpected read in Current Time service\n");
        return false;
    }

    if (handle == mGattHandleCurTime) {
        ptr = (const uint8_t *)&mGattCurTime;
        len = sizeof(mGattCurTime);
    } else if (handle == mGattHandleCurTimeCfg) {
        ptr = (const uint8_t *)&mGattCurTimeCfg;
        len = sizeof(mGattCurTimeCfg);
    } else if (handle == mGattHandleLocalTimeInfo) {
        ptr = (const uint8_t *)&mGattLocalTimeInfo;
        len = sizeof(mGattLocalTimeInfo);
    } else if (handle == mGattHandleReferenceTimeInfo) {
        ptr = (const uint8_t *)&mGattHandleReferenceTimeInfo;
        len = sizeof(mGattHandleReferenceTimeInfo);
    } else {
        logw("Unexpected read from handle %u of svc "GATTHANDLEFMT" from conn "HANDLEFMT"d\n", handle, GATTHANDLECNV(svc), HANDLECNV(who));
        return false;
    }

    if (byteOfst > len) {
        err = ATT_ERROR_INVALID_OFFSET;
    } else {
        ptr += byteOfst;
        len -= byteOfst;
    }
    if (len > wantedLen)
        len = wantedLen;

    logi("testGattSrvValueReadCbk: cid: "UNIQ_FMT"d, transId: "ATT_TRANS_FMT"d, handle: handle: %d\n", ATT_CID_CONV(cid), ATT_TRANS_CONV(transId), handle);

    /* In order to verify the pairing process, ATT_ERROR_INSUFFICIENT_AUTH is
     * returned intentionally so that the remote GATT client will initiate
     * pairing. */
    gattSrvCbkReply(cid, transId, handle, ATT_ERROR_INSUFFICIENT_AUTH, ptr, len);

    /* In order to verify the re-connection to the target device, we immediately
     * re-enable the adv set */
    if (l2cApiGetSelfBtAddr(attSrvCidResolve(cid), &srvAddr)) {
        for (i = 0; i < sizeof(mAddrs); ++i) {
            if (isSameAddr(&srvAddr, &mAddrs[i])) {
                logd("Re-enable adv set %d\n", i);
                hciAdvSetEnable(mAdvSet[i]);
            }
        }
    }

    return true;
}

static bool testGattSrvValueWriteCbk(gatt_service_t svc, l2c_handle_t who, att_cid_t cid, att_trans_t transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    uint8_t err = ATT_ERROR_NONE, i;
    struct bt_addr srvAddr;

    logi("testGattSrvValueWriteCbk: cid: "UNIQ_FMT"d, transId: "ATT_TRANS_FMT"d, handle: handle: %d\n", ATT_CID_CONV(cid), ATT_TRANS_CONV(transId), handle);

    if (handle == mGattHandleCurTimeCfg) {
        mGattCurTimeCfg.cCC = *(const uint8_t *)data;
        mConnTime = cid;
        logi("data: %x, len: %d\n", mGattCurTimeCfg.cCC, len);
        gattSrvCbkReply(cid, transId, handle, err, NULL, 0);
    } else if (handle == mGattHandleLocalTimeInfo) {
        memcpy(&mGattLocalTimeInfo, data, len);
        gattSrvCbkReply(cid, transId, handle, err, NULL, 0);
    } else {
        loge("Unrecognized handle: %d\n", handle);
        return false;
    }

    /* In order to verify the re-connection to the target device, we immediately
     * re-enable the adv set */
    if (l2cApiGetSelfBtAddr(attSrvCidResolve(cid), &srvAddr)) {
        for (i = 0; i < sizeof(mAddrs); ++i) {
            if (isSameAddr(&srvAddr, &mAddrs[i])) {
                logd("Re-enable adv set %d\n", i);
                hciAdvSetEnable(mAdvSet[i]);
            }
        }
    }

    return true;
}

static void testGattSrvIndCbk(gatt_service_t svc, l2c_handle_t who, att_cid_t cid, uint16_t handle, uint8_t evt, uint64_t ref)
{
    int i;
    struct bt_addr srvAddr;

    if (svc != mSvcTime) {
        logw("Unexpected notification callback in Current Time service\n");
        return;
    }

    /* In order to verify the re-connection to the target device, we immediately
     * re-enable the adv set */
    if (l2cApiGetSelfBtAddr(attSrvCidResolve(cid), &srvAddr)) {
        for (i = 0; i < sizeof(mAddrs); ++i) {
            if (isSameAddr(&srvAddr, &mAddrs[i])) {
                logd("Re-enable adv set %d\n", i);
                hciAdvSetEnable(mAdvSet[i]);
            }
        }
    }
    logi("time notification sent: cid: "UNIQ_FMT"d, handle: handle: %d\n", ATT_CID_CONV(cid), handle);
}

static void testGattSrvDiscCbk(gatt_service_t svc, l2c_handle_t who)
{
    //unused
}

static void testGattInitTime() {
    mGattCurTime.exactTime256.dayDateTime.dateTime.year = 2017;
    mGattCurTime.exactTime256.dayDateTime.dateTime.month = 5;
    mGattCurTime.exactTime256.dayDateTime.dateTime.day = 1;
    mGattCurTime.exactTime256.dayDateTime.dateTime.hours = 12;
    mGattCurTime.exactTime256.dayDateTime.dateTime.minutes = 30;
    mGattCurTime.exactTime256.dayDateTime.dateTime.seconds = 5;

    mGattCurTime.adjustReason = 0;
    mGattCurTime.exactTime256.fractions256 = 0;
    mGattCurTime.exactTime256.dayDateTime.dayOfWeek.dayOfWeek = 3;

    mGattCurTimeCfg.cCC = 0;

    mGattLocalTimeInfo.timeZone = 32;   // UTC+8 hours
    mGattLocalTimeInfo.dstOffset = 4;   // daylight time +1 hour
}

static void testIndCb() {
    if (mGattCurTimeCfg.cCC == 0)
        return;

    const uint8_t *ptr = NULL;
    uint16_t len = 0;

    ptr = (const uint8_t *)&mGattCurTime;
    len = sizeof(mGattCurTime);
    gattServiceSendInd(mSvcTime, mConnTime, mGattHandleCurTime, ptr, len, false, 0);
}

static void *testGattSetTime(void *data) {
    uint8_t TICK = 5;

    while (!stopTimeService) {
        sleep(TICK);
        mGattCurTime.exactTime256.dayDateTime.dateTime.seconds += TICK;
        if (mGattCurTime.exactTime256.dayDateTime.dateTime.seconds >= 60) {
            mGattCurTime.exactTime256.dayDateTime.dateTime.seconds -= 60;
            mGattCurTime.exactTime256.dayDateTime.dateTime.minutes += 1;
        }
        logi("time: %d\n", mGattCurTime.exactTime256.dayDateTime.dateTime.seconds);
        testIndCb();
    }

    return NULL;
}

// Sets up current time service as a sample.
static int gattSetup(void)
{
    struct uuid uuid;
    int svc;

    uuidFromUuid16(&uuid, 0x1805); //"current time" service
    svc = gattServiceCreate(&uuid, true, 8, testGattSrvValueReadCbk, testGattSrvValueWriteCbk, testGattSrvIndCbk, testGattSrvDiscCbk);
    if (!svc) {
        loge("failed to alloc svc\n");
        goto out;
    }

    uuidFromUuid16(&uuid, 0x2A2B); //"current time" characteristic
    // GATT_PROP_READ and GATT_PROP_NOTIFY are mandatory.
    // GATT_PROP_WRITE is optional.
    mGattHandleCurTime = gattServiceAddChar(svc, &uuid, GATT_PROP_READ | GATT_PROP_NOTIFY, GATT_PERM_ALL_READ);
    if (!mGattHandleCurTime) {
        loge("Failed to add cur time char\n");
        goto out;
    }

    uuidFromUuid16(&uuid, GATT_UUID_CHAR_CLI_CHAR_CFG); //char config descriptor
    mGattHandleCurTimeCfg = gattServiceAddCharDescr(svc, &uuid, GATT_PERM_ALL_READ | GATT_PERM_ALL_WRITE);
    if (!mGattHandleCurTimeCfg) {
        loge("Failed to add cur time char cfg\n");
        goto out;
    }

    uuidFromUuid16(&uuid, 0x2A0F); //"local time info" characteristic
    mGattHandleLocalTimeInfo = gattServiceAddChar(svc, &uuid, GATT_PROP_READ | GATT_PROP_WRITE, GATT_PERM_ALL_READ | GATT_PERM_ALL_WRITE);
    if (!mGattHandleLocalTimeInfo) {
        loge("Failed to add local time info char\n");
        goto out;
    }

    uuidFromUuid16(&uuid, 0x2A14); //"reference time info" characteristic
    mGattHandleReferenceTimeInfo = gattServiceAddChar(svc, &uuid, GATT_PROP_READ, GATT_PERM_ALL_READ);
    if (!mGattHandleReferenceTimeInfo) {
        loge("Failed to add reference time info char\n");
        goto out;
    }


out:
    return svc;
}

void pairStateObserverCallback(void *observerData, const void *pairStateChange, uniq_t observerId) {
    struct smPairStateChange state;

    if (observerId != mPairStateObserverId || !pairStateChange)
        return;

    state = *(struct smPairStateChange *)pairStateChange;

    logi("Pairing state with "ADDRFMT" changed to %d with err %d\n", ADDRCONV(state.peerAddr),
         state.pairState, state.pairErr);

    // Unpair with the target device once it's paired
    if (state.pairState == SM_PAIR_STATE_PAIRED) {
        sleep(5);
        smUnpair(&state.peerAddr);
    }
}

void passkeyDisplayObserverCallback(void *observerData,
                                    const struct smPasskeyDisplay *passkeyDisplay,
                                    uniq_t observerId) {

    if (observerId != mPasskeyDisplayObserverId || !passkeyDisplay)
        return;

    if (passkeyDisplay->valid)
        logi("Please enter %06d with the keyboard with "ADDRFMT"\n", passkeyDisplay->passkey,
             ADDRCONV(passkeyDisplay->peerAddr));
    else
        logi("The passkey section expired with the keyboard with "ADDRFMT"\n",
             ADDRCONV(passkeyDisplay->peerAddr));
}

void passkeyRequestObserverCallback(void *observerData,
                                    const struct smPasskeyRequest *passkeyRequest,
                                    uniq_t observerId) {
    uint32_t passkey = 0;

    if (observerId != mPasskeyRequestObserverId || !passkeyRequest)
        return;

    if (terminateTimerId) {
        timerCancel(terminateTimerId);
        terminateTimerId = 0;
    }

    if (passkeyRequest->valid) {
        logi("Please enter the passkey with the keyboard to pair with "ADDRFMT":\n",
             ADDRCONV(passkeyRequest->peerAddr));
        if (scanf("%u", &passkey) != 1)
            return;
        smProvidePasskey(&passkeyRequest->peerAddr, true, passkey);
    } else {
        logi("The passkey section expired with the keyboard with "ADDRFMT"\n",
             ADDRCONV(passkeyRequest->peerAddr));
    }

    sleep(3);
    terminate = true;
}

static void gattSvcTraversed(gatt_client_conn_t conn, uniq_t trans, const struct GattTraversedService* data)
{
    uint32_t i, j;

    if (!data)
        logi("Traversal failed\n");
    else {
        logi("SERVICE "UUIDFMT" in range 0x%04x - 0x%04x:\n", UUIDCONV(data->uuid), data->firstHandle, data->lastHandle);
        if (data->numInclSvcs) {
            logi("  INCLUDED SERVICES (%u):\n", data->numInclSvcs);
            for (i = 0; i < data->numInclSvcs; i++)
                logi("    iSVC "UUIDFMT" with range 0x%04x - 0x%04x (defined at 0x%04x)\n", UUIDCONV(data->inclSvcs[i].uuid), data->inclSvcs[i].firstHandle, data->inclSvcs[i].lastHandle, data->inclSvcs[i].includeDefHandle);
        }
        if (data->numChars) {
            logi("  CHARACTERISTICS (%u):\n", data->numChars);
            for (i = 0; i < data->numChars; i++) {
                logi("    CHAR "UUIDFMT" with range 0x%04x - 0x%04x (value   at 0x%04x)\n", UUIDCONV(data->chars[i].uuid), data->chars[i].firstHandle, data->chars[i].lastHandle, data->chars[i].valHandle);
                if (data->chars[i].numDescrs) {
                    logi("      DESCRIPTORS (%u):\n", data->chars[i].numDescrs);
                    for (j = 0; j < data->chars[i].numDescrs; j++)
                        logi("          DESCR "UUIDFMT" at 0x%04x\n", UUIDCONV(data->chars[i].descrs[j].uuid), data->chars[i].descrs[j].handle);
                }
            }
        }
    }
    sem_post(&mGattTestSem);
}

static void gattSvcFoundCbk(gatt_client_conn_t conn, uniq_t trans, const struct uuid *uuid, bool primary, uint16_t firstHandle, uint16_t numHandles, uint8_t status)
{
    struct ServiceList *t;

    if (status != GATT_CLI_STATUS_OK) {
        logi("enum status %u\n", status);
        return;
    }

    if (!uuid) {
        logi("enumeration ends\n");
        sem_post(&mGattTestSem);
        return;
    }

    logi("Found %s service "UUIDFMT" with handles 0x%04x+0x%04x\n", primary ? "primary" : "secondary", UUIDCONV(*uuid), firstHandle, numHandles);

    t = calloc(sizeof(struct ServiceList), 1);
    t->uuid = *uuid;
    t->start = firstHandle;
    t->end = firstHandle + numHandles - 1;
    t->next = mGattSvcs;
    mGattSvcs = t;
}

static void gattConnectedCbkGattEnum(gatt_client_conn_t conn, uint8_t status)
{
    logi("Connect status: %u\n", status);

    if (status != GATT_CLI_STATUS_OK)
        return;

    //list services
    mGattSvcs = NULL;
    logi("service enum = %u\n", gattClientEnumServices(conn, true, 0, gattSvcFoundCbk));
}

static void gattConnectedCbkHidTest(gatt_client_conn_t conn, uint8_t status)
{
    logi("GATT for HID connect status: %u\n", status);

    if (status == GATT_CLI_STATUS_OK) {
        mHidConn = btleHidAttach(conn);
        mBattSvcCon = gattSvcBattAttach(conn);
        mMiscInfoSvcCon = gattSvcMiscInfoAttach(conn);
    }

    //either way unblock main thread
    sem_post(&mGattTestSem);
}

static void battSvcStateCbk(gatt_svc_batt_conn_t handle, uint8_t state)
{
    logi("BattSvc state: %u\n", state);

    if (state == BTLE_BATT_SVC_CONN_STATE_UP) {
        logi(" BattSvc async support: %s\n", gattSvcBattIsNotifSupported(mBattSvcCon) ? "YES" : "NO");
        logi(" BattSvc read requested: %s\n", gattSvcBattRequestRead(mBattSvcCon) ? "OK" : "ERROR");
    }
}

static void battSvcRxCbk(gatt_svc_batt_conn_t handle, uint8_t percent, bool byRequest)
{
    logi("Batt state: %u%% (%s)\n", percent, byRequest ? "by request" : "async update");
}

static void miscInfoSvcStateCbk(gatt_svc_misc_info_conn_t handle, uint8_t state)
{
    uint32_t i;
    logi("Misc Info state: %u\n", state);

    if (state == BTLE_MISC_INFO_SVC_CONN_STATE_UP) {
        for (i = 0; i < GattMiscInfoEnumMax; i++) // we expect first to fail (as it is invalid - this tests that)
            logi("Misc Info Request %u: %s\n", i, gattSvcMiscInfoRequestRead(handle, i) ? "YES" : "NO");
    }
}

static void showMiscInfoStr(const char *name, sg data)
{
    char* t;

    if (!data)
        return;

    t = calloc(1, sgLength(data) + 1);
    sgSerialize(data, 0, sgLength(data), t);
    sgFree(data);
    fprintf(stderr, "  %s: '%s'\n", name, t);
    free(t);
}

static void miscInfoSvcRxCbk(gatt_svc_misc_info_conn_t handle, enum GattSvcMiscInfoType type, sg data)
{
    uint8_t buf[16];

    if (!data)
        return;

    switch(type) {
        case GattMiscInfoTypeManufNameString:
            showMiscInfoStr("Manufacturer Name", data);
            return;
        case GattMiscInfoTypeModelNumberString:
            showMiscInfoStr("Model Number", data);
            return;
        case GattMiscInfoTypeSerialNumberString:
            showMiscInfoStr("Serial Number", data);
            return;
        case GattMiscInfoTypeHardwareRevisionString:
            showMiscInfoStr("HW Revision", data);
            return;
        case GattMiscInfoTypeFirmwareRevisionString:
            showMiscInfoStr("FW Revision", data);
            return;
        case GattMiscInfoTypeSoftwareRevisionString:
            showMiscInfoStr("SW Revision", data);
            return;
        case GattMiscInfoTypeDeviceName:
            showMiscInfoStr("Device Name", data);
            return;
        case GattMiscInfoTypeSystemID:
            if (!sgSerializeCutFront(data, buf, sizeof(uint64_t)) || sgLength(data))
                fprintf(stderr, "  Invalid SystemID value format\n");
            else
                fprintf(stderr, "  System ID: 0x%016llx\n", (unsigned long long)utilGetLE64(buf));
            break;
        case GattMiscInfoTypeIeee11073_20601regCertList:
            fprintf (stderr, "  I have no idea how to show IEEE cert lists, but we got one %u bytes long\n", sgLength(data));
            break;
        case GattMiscInfoTypePnpID:
            if (!sgSerializeCutFront(data, buf, 7) || sgLength(data))
                fprintf(stderr, "  Invalid PnP ID value format\n");
            else
                fprintf(stderr, "  PnP ID: {src=%u,vid=0x%04x,pid=0x%04x,ver=0x%04x}\n", buf[0], utilGetLE16(buf + 1), utilGetLE16(buf + 3), utilGetLE16(buf + 5));
            break;
        case GattMiscInfoTypeAppearance:
            if (!sgSerializeCutFront(data, buf, 2) || sgLength(data))
                fprintf(stderr, "  Invalid appearance value format\n");
            else
                fprintf(stderr, "  Appearance: 0x%04x (%s,%s)\n", utilGetLE16(buf), getAppearanceTypeStr(utilGetLE16(buf)), getAppearanceSubtypeStr(utilGetLE16(buf)));
            break;
        default:
            fprintf(stderr, "  Got unkwnon dev info type %u\n", type);
            break;
    }
    sgFree(data);
}

static bool eirNameMatch(const uint8_t *data, uint32_t len, const char *name)
{
    while (len) {


        logi("    len is %u\n", len);


        uint8_t typ, pieceLen = *data++;
        len--;


        logi("    len is %u pieceLen is %u\n", len, pieceLen);

        if (len < pieceLen || !pieceLen)
            break;
        typ = *data++;
        len--;
        pieceLen--;
        logi("  adv chunk type 0x%x with %u bytes payload\n", typ, pieceLen);
        if ((typ == 8 || typ == 9) && pieceLen == strlen(name) && !memcmp(data, name, pieceLen))
            return true;
        len -= pieceLen;
        data += pieceLen;
    }

    return false;
}

static void terminateCbk(uniq_t which,  uint64_t cbkData) {
    if (which != terminateTimerId)
        return;
    terminate = true;
}

//for details see https://www.bluetooth.com/specifications/gatt/viewer?attributeXmlFile=org.bluetooth.characteristic.gap.appearance.xml
static const char* mLeAppearanceClasses[] =
    {
        "Unknown", "Phone", "Computer", "Watch", "Clock", "Display", "Remote Control", "Eyeglasses",
        "Tag", "Keyring", "Media Player", "Barcode Scanner", "Thermometer", "Heart Rate Sensor", "Blood Pressure", "HID",
        "Glucose Meter", "Running / Walking Sensor", "Cycling",
        [49] = "Pulse Oximeter",
        [50] = "Weight Scale",
        [51] = "Personal Mobility Device",
        [52] = "Continuous Glucose Monitor",
        [53] = "Insulin Pump",
        [54] = "Medication Delivery",
        [81] = "Outdoor Sports Activity",
    };

static const char* getAppearanceTypeStr(uint32_t appVal)
{
    uint32_t cls = appVal >> 6;

    return (cls >= sizeof(mLeAppearanceClasses) / sizeof(*mLeAppearanceClasses) || !mLeAppearanceClasses[cls]) ? "Unknown" : mLeAppearanceClasses[cls];
}

static const char* getAppearanceSubtypeStr(uint32_t appVal)
{
    static const char* hidSubclasses[] = {"Generic", "Keyboard", "Mouse", "Joystick", "Gamepad", "Digitizer Tablet", "Card Reader", "Digital Pen", "Barcode Scanner", NULL};
    static const char* outdoorSubclasses[] = {"Generic", "Location Display Device", "Location and Navigation Display Device", "Location Pod", "Location and Navigation Pod", NULL};
    static const char** subclasses[] = {[15] = hidSubclasses, [81] = outdoorSubclasses,};
    const char** t;

    uint32_t cls = appVal >> 6;
    uint32_t subclass = appVal & 0x3F;

    if (cls >= sizeof(subclasses) / sizeof(*subclasses) || !subclasses[cls])
        return "Unknown";

    //search for sentinel value or proper place
    for (t = subclasses[cls]; *t && subclass; t++, subclass--);

    return *t ? *t : "Unknown";
}

static void interpAvdData(const uint8_t *data, uint32_t len)
{

    //for details, see https://www.bluetooth.com/specifications/assigned-numbers/generic-access-profile
    while (len >= 2) {
        uint32_t val;
        uint8_t i, typ, sz;
        struct uuid uuid;
        char *buf;

        sz = *data++;

        if (!sz || sz > len - 1)
            break;
        typ = *data++;
        len -= 2;
        sz--;

        switch (typ) {
        case 0x01:
            if (sz != 1)
                goto out;
            logi("\t\tFLAGS: 0x%02x\n", data[0]);
            break;
        case 0x02:
        case 0x03:
            logi("\t\tUUID16s (%scomplete):\n", (typ & 1) ? "" : "in");
            if (sz % SIZEOF_UUID_16)
                goto out;
            for (i = 0; i < sz; i += SIZEOF_UUID_16)
                logi("\t\t\t0x%04x\n", utilGetLE16(data + i));
            break;
        case 0x04:
        case 0x05:
            logi("\t\tUUID32s (%scomplete):\n", (typ & 1) ? "" : "in");
            if (sz % SIZEOF_UUID_32)
                goto out;
            for (i = 0; i < sz; i += SIZEOF_UUID_32)
                logi("\t\t\t0x%08x\n", utilGetLE32(data + i));
            break;
        case 0x06:
        case 0x07:
            logi("\t\tUUID128s (%scomplete):\n", (typ & 1) ? "" : "in");
            if (sz % sizeof(struct uuid))
                goto out;
            for (i = 0; i < sz; i += sizeof(struct uuid)) {
                uuidReadLE(&uuid, data + i);
                logi("\t\t\t"UUIDFMT"\n", UUIDCONV(uuid));
            }
            break;
        case 0x08:
        case 0x09:
            buf = malloc(sz + 1);
            memcpy(buf, data, sz);
            buf[sz] = 0;
            logi("\t\tNAME (%scomplete): <<%s>>\n", (typ & 1) ? "" : "in", buf);
            free(buf);
            break;
        case 0x0a:
            if (sz != 1)
                goto out;
            logi("\t\tTX PWR: %d dBm\n", (int8_t)data[0]);
            break;
        case 0x0d:
            if (sz != 3)
                goto out;
            logi("\t\tCLASS: 0x%06x\n", utilGetLE24(data));
            break;
        case 0x11:
            if (sz != 1)
                goto out;
            logi("\t\tSEC MGR FLAGS: 0x%02x\n", data[0]);
            break;
        case 0x12:
            if (sz != 4)
                goto out;
            logi("\t\tSLAVE CONNECTION INTERVAL PREFERENCE: 0x%04x - 0x%04x\n", utilGetLE16(data + 0), utilGetLE16(data + 2));
            break;
        case 0x19:
            if (sz != 2)
                goto out;
            val = utilGetLE16(data);
            logi("\t\tAPPEARANCE: 0x%04x (%s,%s)\n", val, getAppearanceTypeStr(val), getAppearanceSubtypeStr(val));
            break;
        case 0x1b:
            if (sz != 7)
                goto out;
            logi("\t\tADDRESS: %s %02x:%02x:%02x:%02x:%02x:%02x\n", data[0] ? (data[0] == 1 ? "Random" : "Unknown") : "Public", data[6], data[5], data[4], data[3], data[2], data[1]);
            break;
        case 0x16:
        case 0xff:
            if (sz < 2)
                goto out;
            logi("\t\t%s 0x%04x (%u bytes)\n", (typ == 0xFF) ? "MANUF DATA FOR MANUF" : "SERVICE DATA FOR SVC", utilGetLE16(data), sz - 2);
            i = 2;
        showdata:
            buf = malloc(sz * 4);
            buf[0] = 0;
            for (; i < sz; i++)
                sprintf(buf + strlen(buf), "%02x%s", data[i], (i == sz - 1) ? "" : ", ");
            logi("\t\t\t%s\n", buf);
            free(buf);
            break;
        default:
            logi("\t\tTYPE 0x%02x, %u bytes:\n", typ, sz);
            i = 0;
            goto showdata;
        }
        len -= sz;
        data += sz;
    }
out:
    if (len)
        logi("\tLEFTOVER %u bytes!\n", len);
}

int main(int argc, char** argv)
{
    static const uint8_t mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    static const struct bt_addr addr = {
      .type = BT_ADDR_TYPE_LE_RANDOM, .addr = {0xFF, 0x8E, 0xFC, 0x0A, 0x11, 0xFF}}; //adv set 0
    sem_t sem;

    fprintf(stderr, "NewBlue test\n");
    logi("LOG UP\n");

    if (sem_init(&mGattTestSem, 0, 0)) {
        loge("sem init failed\n");
        return -1;
    }

    if (sem_init(&sem, 0, 0)) {
        loge("sem init failed\n");
        return -1;
    }

#ifdef TEST_DEBUG
    vendorLogEnable(true);
#endif

    if (!hciUp(mac, readyForUpCbk, NULL)) {
        loge("HCI up fail\n");
        return -1;
    }

    logi("waiting for stack up\n");
    while(!hciIsUp())
        sleep(1);
    logi("stack is up\n");

    l2cInit();

    if (!attInit())
        loge("Failed to init ATT\n");
    if (!gattProfileInit())
        loge("Failed to init GATT\n");
    if (!gattBuiltinInit())
        loge("Failed to init GATT services\n");

    if (!smInit()) {
        loge("Failed to init SM\n");
        goto cleanup;
    }

    timersInit();

    //initialize addresses for adv sets
    mAddrs[0] = addr;
    if (!smGenResolvableAddr(&mAddrs[1])) {
        loge("Failed to generate resolvable private address for adv set\n");
        goto cleanup;
    }
    if (!smGenNonResolvableAddr(&mAddrs[2])) {
        loge("Failed to generate non-resolvable private address for adv set\n");
        goto cleanup;
    }

    if (argc == 2 && (!strcmp(argv[1], "scan") || !strcmp(argv[1], "scanconnect") || !strcmp(argv[1], "hidtest"))) {
        /*
         * Option: scan, scanconnect, hidtest
         * Use:    start LE scanning for a while and terminate. in case of scanconnect, connect and enumerate GATT things, for "hidtest", try the hid test
         */

        bool foundDev = false, wantConnect = !strcmp(argv[1], "scanconnect"), doHidTest = !strcmp(argv[1], "hidtest");
        struct LeDiscDev *leDevices = NULL, *t;
        gatt_client_conn_t con = 0;
        struct bt_addr connTo;
        uniq_t h;

        logi("Trying some LE discovery%s...\n", (wantConnect || doHidTest) ? " and connection" : "");
        h = hciDiscoverLeStart(leDiscoveryCbk, &leDevices, true, false);
        if (h) {
            sleep((wantConnect || doHidTest) ? 5 : 20);
            fprintf(stderr, "\n");
            logi("discovery stop = %s\n", hciDiscoverLeStop(h) ? "OK" : "FAIL");

            while (leDevices) {
                char str[128];
                t = leDevices;
                leDevices = leDevices->next;

                logi("Device %c %02X:%02X:%02X:%02X:%02X:%02X\n", t->randomAddr ? 'R' : 'P',
                     t->mac[5], t->mac[4], t->mac[3], t->mac[2], t->mac[1], t->mac[0]);
                if (t->rssi != HCI_RSSI_UNKNOWN)
                    logi("\tRSSI: %d dBm\n", t->rssi);
                else
                    logi("\tRSSI unknown\n");
                logi("\tADV_TYPE: %d, RSP? %s\n", t->advType, t->haveRsp ? "YES" : "NO");

                logData(str, t->advData, t->advLen);
                logi("\tADV %s\n", str);
                interpAvdData(t->advData, t->advLen);

                if (t->haveRsp) {
                    logData(str, t->rspData, t->rspLen);
                    logi("\tRSP %s\n", str);
                    interpAvdData(t->rspData, t->rspLen);
                }

                if (wantConnect || doHidTest) {   //is this the one we shoudl connect to???

                    static const char *connNameMatch = "PC120A";

                    if (eirNameMatch(t->advData, t->advLen, connNameMatch) || (t->haveRsp && eirNameMatch(t->rspData, t->rspLen, connNameMatch))) {
                      foundDev = true;
                      memcpy(connTo.addr, t->mac, BT_MAC_LEN);
                      connTo.type = t->randomAddr ? BT_ADDR_TYPE_LE_RANDOM : BT_ADDR_TYPE_LE_PUBLIC;
                      logi("Found device to connect to: "ADDRFMT"\n", ADDRCONV(connTo));
                    }
                }
                free(t);
            }

            if (!foundDev)
                logi("Desired device not found. Sorry\n");
            else if (doHidTest) {

                gattSvcBattInit(battSvcStateCbk, battSvcRxCbk);
                btleHidInit(hidConnStateCbk, hidReportRxCbk);
                gattSvcMiscInfoInit(miscInfoSvcStateCbk, miscInfoSvcRxCbk);

                con = gattClientConnect(&connTo, gattConnectedCbkHidTest);
                if (!con)
                    logw("Failed to try to form a GATT connection!\n");
                else {
                    logi("Gatt connection handle: "UNIQ_FMT"x\n", UNIQ_CONV(con));

                    sem_wait(&mGattTestSem);
                    if (mHidConn) { //connection in progress - wait some more
                        sem_wait(&mGattTestSem);
                        logi("HID connected. press a key to exit\n");
                        getchar();
                        btleHidDetachFromGatt(con);
                        gattClientDisconnect(con);
                    }
                }
            }
            else if (wantConnect) {

                con = gattClientConnect(&connTo, gattConnectedCbkGattEnum);
                if (!con)
                    logw("Failed to try to form a GATT connection!\n");
                else {
                    logi("Gatt connection handle: "UNIQ_FMT"x\n", UNIQ_CONV(con));

                    //service enum will start in gattConnectedCbkGattEnum() and we'll wait for it here
                    sem_wait(&mGattTestSem);
                    logi("Service enum ends...\n");

                    //list details of each service
                    while (mGattSvcs) {
                        struct ServiceList *svc = mGattSvcs;
                        mGattSvcs = mGattSvcs->next;

                        logi("ENUMERATING SERVICE "UUIDFMT" (known to be in range 0x%04x - 0x%04x): %u\n", UUIDCONV(svc->uuid), svc->start, svc->end, gattClientUtilFindAndTraversePrimaryService(con, &svc->uuid, 0, gattSvcTraversed));
                        free(svc);
                        sem_wait(&mGattTestSem);
                    }
                }
            }

        } else
            loge("Failed to do LE discovery\n");
    }
    else if ((argc == 2 && !strcmp(argv[1], "pair")) || (argc == 3 && !strcmp(argv[1], "pair"))) {
        /*
         * Option: pair
         * Use:    set up a GATT connection with the device with mAddrs[0] or the provided address
         *         and start pairing
         */

        bool found = false;
        uniq_t scanId;
        unsigned i, round = 3;
        struct LeDiscDev *leDevices = NULL, *t;
        struct smPairSecurityRequirements secReqs = {.bond = true, .mitm = true};
        regex_t regex;

        if (argc == 3) {
            if (regcomp(&regex, "^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$", REG_EXTENDED) != 0 ||
                regexec(&regex, argv[2], 0, NULL, 0) != 0) {
                loge("Invalid mac address provided\n");
                goto cleanup;
            }
            regfree(&regex);

            sscanf(argv[2], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mInputDevAddr.addr[5],
                   &mInputDevAddr.addr[4], &mInputDevAddr.addr[3], &mInputDevAddr.addr[2],
                   &mInputDevAddr.addr[1], &mInputDevAddr.addr[0]);
            mInputDevAddr.type = BT_ADDR_TYPE_LE_RANDOM;
        }

        // register as an observer to respond to passkey display
        mPasskeyDisplayObserverId =
            smRegisterPasskeyDisplayObserver(NULL, passkeyDisplayObserverCallback);
        if (!mPasskeyDisplayObserverId) {
            loge("Failed to register as an observer to passkey display\n");
            goto cleanup;
        }

        // register as an observer to pairing state changes
        mPairStateObserverId = smRegisterPairStateObserver(NULL, pairStateObserverCallback);
        if (!mPairStateObserverId) {
            loge("Failed to register as an observer to pairing state changes\n");
            goto cleanup;
        }

        logi("Scanning for device "ADDRFMT" ...\n",
             ADDRCONV(argc == 3 ? mInputDevAddr : mAddrs[0]));
        for (i = 0; i < round; ++i) {
            scanId = hciDiscoverLeStart(leDiscoveryCbk, &leDevices, true, false);
            if (!scanId) {
                loge("Failed to do LE discovery\n");
                break;
            }

            // scan for 5 seconds
            sleep(5);
            fprintf(stderr, "\n");
            logi("discovery stop = %s\n", hciDiscoverLeStop(scanId) ? "OK" : "FAIL");

            // find the target device to connect to and pair with it
            while (leDevices) {
                t = leDevices;
                leDevices = leDevices->next;
                logi("Device %c %02X:%02X:%02X:%02X:%02X:%02X\n", t->randomAddr ? 'R' : 'P',
                     t->mac[5], t->mac[4], t->mac[3], t->mac[2], t->mac[1], t->mac[0]);
                if ((found = !memcmp((argc == 3 ? mInputDevAddr.addr : mAddrs[0].addr), t->mac,
                                     sizeof(t->mac)))) {
                    found = true;
                    break;
                } else {
                    free(t);
                    continue;
                }
            }

            if (found) {
                logi("Target device found, try to pair with device "ADDRFMT" ...\n",
                     ADDRCONV(argc == 3 ? mInputDevAddr : mAddrs[0]));
                smPair((argc == 3 ? &mInputDevAddr : &mAddrs[0]), &secReqs);
                break;
            }

            // reset discovery session ID
            scanId = 0;
        }

        logi("waiting...press a key\n");
        getchar();
    }
    else if (argc == 1) {
        /*
         * Option: none
         * Use:    set up GATT server with Current Time Service and start 3 advertisements and wait
         *         for either pairing or connection request
         */

        unsigned i;
        int8_t txPower = 0;
        // mAdvSet[0]
        static const uint8_t data[] = {
            2, HCI_EIR_TYPE_FLAGS, 2, /* flags - general discoverable */
            5, HCI_EIR_TYPE_FULL_NAME, 'N', 'B', 't', '2', /* name - "NBt2" */
            3, HCI_EIR_TYPE_INCOMPL_LIST_UUID16, 0x05, 0x18, /* incomplete uuid list - {time service} */
            5, HCI_EIR_FLAVE_CONN_INTS, 0x10, 0x00, 0xff, 0x00, /* relatively large range of relatively large connection intervals accepted */
        };

        mSvcTime = gattSetup();
        logi("gattSetup: svc="GATTHANDLEFMT"\n", GATTHANDLECNV(mSvcTime));
        if (!gattServiceStart(mSvcTime, true, true))
            loge("Failed to start service: "GATTHANDLEFMT"\n", GATTHANDLECNV(mSvcTime));
        logd("Current time service created as svc "GATTHANDLEFMT" at handle 0x%04X\n", GATTHANDLECNV(mSvcTime), gattServiceGetHandleBaseBySvc(mSvcTime));

        testGattInitTime();

        pthread_t setTimeThread;
        if (pthread_create(&setTimeThread, NULL, testGattSetTime, NULL)) {
            loge("Failed to create pthread for current time service.\n");
            return -1;
        }

        gattBuiltinSetDevName("SuperLongDeviceNameThatWillProbablyNotFitIntoASinglePacketEver");
        gattBuiltinSetPreferredConnParamsWhenSlave(20, 30, 40, 400);
        gattBuiltinSetAppearance(GAP_APPEAR_GENERIC_WATCH);

        // register as an observer to respond to passkey display
        mPasskeyRequestObserverId =
            smRegisterPasskeyRequestObserver(NULL, passkeyRequestObserverCallback);
        if (!mPasskeyRequestObserverId) {
            loge("Failed to register as an observer to passkey request\n");
            goto cleanup;
        }

        //now let's try some multi-adv
        for (i = 0; i < sizeof(mAdvSet) / sizeof(*mAdvSet); i++) {
            static const uint8_t advData[] = {2, HCI_EIR_TYPE_FLAGS, 2, /* flags - general discoverable */};
            uint8_t scanData[] = {6, HCI_EIR_TYPE_FULL_NAME, 'N', 'B', 't', '1', '0' + i, /* name - "NBt10" and up */};

            mAdvSet[i] = hciAdvSetAllocate();
            if (!mAdvSet[i]) {
                loge("Failed to create multi set %u\n", i);
                break;
            }
            if (!i)
                txPower = -10;
            else if (i == 1)
                txPower = HCI_ADV_TX_PWR_LVL_DONT_CARE;
            else
                txPower = -10;

            if (!hciAdvSetSetAdvParams(
                    mAdvSet[i], 0x0040, 0x0100, HCI_ADV_TYPE_ADV_IND,
                    HCI_ADV_OWN_ADDR_TYPE_RANDOM, NULL,
                    HCI_ADV_CHAN_MAP_USE_CHAN_37 | HCI_ADV_CHAN_MAP_USE_CHAN_38 | HCI_ADV_CHAN_MAP_USE_CHAN_39,
                    HCI_ADV_FILTER_POL_SCAN_ALL_CONNECT_ALL, txPower))
                loge("Failed to set LE adv set %u params\n", i);

            if (!hciAdvSetConfigureData(mAdvSet[i], false, !i ? data : advData, !i ? sizeof(data) : sizeof(advData)))
                loge("Failed to set LE adv set %u adv data\n", i);

            if (i && !hciAdvSetConfigureData(mAdvSet[i], true, scanData, sizeof(scanData)))
                loge("Failed to set LE adv set %u scan data\n", i);

            if (!hciAdvSetOwnRandomAddr(mAdvSet[i], &mAddrs[i]))
                loge("Failed to set LE adv set %u random addr\n", i);

            if (!hciAdvSetEnable(mAdvSet[i]))
                loge("Failed to try to enable adv set %u\n", i);
        }

        //print tx power for each adv in a set
        for (i = 0; i < sizeof(mAdvSet) / sizeof(*mAdvSet); i++) {
            int8_t power;

            if (!mAdvSet[i])
                continue;
            if (!hciAdvSetGetCurTxPowerLevel(mAdvSet[i], &power))
                loge("Failed to get adv power for set %u\n", i);
            else
                logi(" adv set %u is at %d dBm with addr "ADDRFMT"\n", i, power, ADDRCONV(mAddrs[i]));
        }

        //if there is no passkey request callback, the timer will be fired after 20 seconds
        terminateTimerId = timerSet(20000 /* 20 secs */, terminateCbk, 0);
        if (!terminateTimerId) {
            logw("Failed to set timer for termination");
            terminate = true;
        }

        //wait for either timer with terminateTimerId to fire or the passkey request callback
        while (!terminate)
           sleep(1);

        logi("waiting...press a key\n");
        getchar();

        //disable and free adv sets
        for (i = 0; i < sizeof(mAdvSet) / sizeof(*mAdvSet); i++) {
            if (!mAdvSet[i])
                continue;
            if (!hciAdvSetDisable(mAdvSet[i]))
                loge("Failed to disable adv %u\n", i);
            if (!hciAdvSetFree(mAdvSet[i]))
                loge("Failed to free adv %u\n", i);
        }

        stopTimeService = true;
        if (pthread_join(setTimeThread, NULL))
            loge("Failed to join the pthread of current time service.\n");
        logi("The pthread of current time service was terminated.\n");

        if (mSvcTime) {
            gattServiceStop(mSvcTime);
            gattServiceDestroy(mSvcTime);
        }
    }

cleanup:
    logi("bringing down\n");
    timersDeinit();
    smDeinit();
    gattBuiltinDeinit();
    gattProfileDeinit();
    attDeinit();
    l2cDeinit();
    hciDown();

    logi("done\n");

    return 0;
}












void aapiAdapterStateChanged(bool on)
{
    logd("aapiAdapterStateChanged(%d)\n", on);
}

void aapiAdapterProperties(bt_status_t status, int num_properties, const bt_property_t *properties)
{
    logd("aapiAdapterProperties(...)\n");
}

void aapiRemoteDevProperties(bt_status_t status, const bt_bdaddr_t *bd_addr, int num_properties, const bt_property_t *properties)
{
    logd("aapiRemoteDevProperties(...)\n");
}

void aapiDevDiscoveredCbk(int num_properties, const bt_property_t *properties)
{
    logd("aapiDevDiscoveredCbk(...)\n");
}

void aapiDiscoveryStateChanged(bool on)
{
    logd("aapiDiscoveryStateChanged(%d)\n", on);
}

void aapiBondStateChangedCbk(const struct bt_addr *peer, uint8_t state)
{
    logd("aapiBondStateChangedCbk(...)\n");
}

void aapiAclStateChanged(bt_status_t status, const struct bt_addr *peer, bool up)
{
    logd("aapiAclStateChanged(..., %d)\n", up);
}

void aapiDutModeEventCbk(uint16_t opcode, const uint8_t *buf, uint8_t len)
{
    logd("aapiDutModeEventCbk(...)\n");
}

void aapiLeTestModeEventCbk(bt_status_t status, uint16_t num_packets)
{
    logd("aapiLeTestModeEventCbk(...)\n");
}


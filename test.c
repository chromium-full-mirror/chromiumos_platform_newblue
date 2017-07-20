#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "gatt-builtin.h"
#include "vendorLib.h"
#include "persist.h"
#include "timer.h"
#include "l2cap.h"
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



static uint8_t gStatus = 0;


static void testHciScoRxCbk(hci_conn_t aclConn, const void *data, uint8_t len, uint8_t lossAmount)
{
   static const char *lossAmtStr[] = {"none", "some", "unknown", "all"};

   logi("TEST SCO RX %ub, loss: %s\n", len, lossAmtStr[lossAmount]);
}

static void opDoneCbk(void *cbkData, uint8_t status)
{
    sem_t *sem = (sem_t *)cbkData;

    gStatus = status;
    logi("status=%d\n", status);
    sem_post(sem);
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

static int mSvcTime = 0;
static int mConnTime = 0;
static bool stopTimeService = false;

uint16_t mGattHandleCurTime = 0;
uint16_t mGattHandleCurTimeCfg = 0;
uint16_t mGattHandleLocalTimeInfo = 0;
uint16_t mGattHandleReferenceTimeInfo = 0;

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

static bool testGattSrvValueReadCbk(int svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t wantedLen)
{
    uint8_t err = ATT_ERROR_NONE;
    const uint8_t *ptr = NULL;
    uint16_t len = 0;

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
        logw("Unexpected read from handle %u of svc %u from conn "HANDLEFMT"\n", handle, svc, HANDLECNV(who));
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

    logi("testGattSrvValueReadCbk: cid: %d, transId: %d, handle: handle: %d\n", cid, transId, handle);

    gattSrvCbkReply(cid, transId, handle, err, ptr, len);
    return true;
}

static bool testGattSrvValueWriteCbk(int svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    uint8_t err = ATT_ERROR_NONE;

    logi("testGattSrvValueWriteCbk: cid: %d, transId: %d, handle: handle: %d\n", cid, transId, handle);

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

    return true;
}

static void testGattSrvIndCbk(int svc, l2c_handle_t who, int cid, uint16_t handle, uint8_t evt, uint64_t ref)
{
    if (svc != mSvcTime) {
        logw("Unexpected notification callback in Current Time service\n");
        return;
    }

    logi("time notification sent: cid: %d, handle: handle: %d\n", cid, handle);
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
    uint8_t seconds;
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
    svc = gattServiceCreate(&uuid, true, 8, testGattSrvValueReadCbk, testGattSrvValueWriteCbk, testGattSrvIndCbk);
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

int main(int argc, char** argv)
{
    static const uint8_t mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const bool vTrue = true;
    const bool vFalse = false;
    unsigned i;
    int svc;
    sem_t sem;
    uniq_t h;
    // "8ce255c0-200a-11e0-ac64-0800200c9a66", the UUID used by BluetoothChat sample program
    uint8_t uuid[] = { 0x8c, 0xe2, 0x55, 0xc0, 0x20, 0x0a, 0x11, 0xe0,
                       0xac, 0x64, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66 };

    fprintf(stderr, "NewBlue test\n");
    logi("LOG UP\n");

    if (sem_init(&sem, 0, 0)) {
        loge("sem init failed\n");
        return -1;
    }

#ifdef TEST_DEBUG
    vendorLogEnable(true);
#endif

    persistLoad();
    timersInit();
    if (!hciUp(mac)) {
        loge("HCI up fail\n");
        return -1;
    }

    if (argc == 2 && !strcmp(argv[1], "scan")) {

        //try LE discovery
        {
            struct LeDiscDev *leDevices = NULL, *t;

            logi("Trying some LE discovery...\n");
            h = hciDiscoverLeStart(leDiscoveryCbk, &leDevices, true, false);
            if (h) {
                sleep(20);
                fprintf(stderr, "\n");
                logi("discovery stop = %s\n", hciDiscoverLeStop(h) ? "OK" : "FAIL");

                while (leDevices) {
                    char str[128];
                    t = leDevices;
                    leDevices = leDevices->next;

                    logi("Device %c %02X:%02X:%02X:%02X:%02X:%02X\n", t->randomAddr ? 'R' : 'P', t->mac[5], t->mac[4], t->mac[3], t->mac[2], t->mac[1], t->mac[0]);
                    if (t->rssi != HCI_RSSI_UNKNOWN)
                        logi("\tRSSI: %d dBm\n", t->rssi);
                    else
                        logi("\tRSSI unknown\n");
                    logi("\tADV_TYPE: %d, RSP? %s\n", t->advType, t->haveRsp ? "YES" : "NO");

                    logData(str, t->advData, t->advLen);
                    logi("\tADV %s\n", str);

                    if (t->haveRsp) {
                        logData(str, t->rspData, t->rspLen);
                        logi("\tRSP %s\n", str);
                    }
                    free(t);
                }

            } else
                loge("Failed to do LE discovery\n");
        }
    }

    l2cInit();

    if (!attInit())
        loge("Failed to init ATT\n");
    if (!gattProfileInit())
        loge("Failed to init GATT\n");
    if (!gattBuiltinInit())
        loge("Failed to init GATT services\n");


    mSvcTime = gattSetup();
    logi("gattSetup: svc=%d\n", mSvcTime);
    if (!gattServiceStart(mSvcTime, true, true))
        loge("Failed to start service: %d\n", mSvcTime);
    logd("Current time service created as svc %u at handle 0x%04X\n", mSvcTime, gattServiceGetHandleBaseBySvc(mSvcTime));

    testGattInitTime();

    pthread_t setTimeThread;
    if (pthread_create(&setTimeThread, NULL, testGattSetTime, NULL)) {
        loge("Failed to create pthread for current time service.\n");
        return -1;
    }


    gattBuiltinSetDevName("SuperLongDeviceNameThatWillProbablyNotFitIntoASinglePacketEver");
    gattBuiltinSetPreferredConnParamsWhenSlave(20, 30, 40, 400);
    gattBuiltinSetAppearance(GAP_APPEAR_GENERIC_WATCH);

    // Set advertising parameters.
    uint8_t peerAddress[BT_MAC_LEN] = {0x00};
    if (!hciLeSetAdvParams(0x00c8,  // uint16_t advIntervalMin;
                           0x0190,  // uint16_t advIntervalMax;
                           HCI_ADV_TYPE_ADV_IND,
                           HCI_ADV_OWN_ADDR_TYPE_PUBLIC,
                           HCI_ADV_OWN_ADDR_TYPE_PUBLIC,
                           peerAddress,
                           HCI_ADV_CHAN_MAP_USE_CHAN_37 | HCI_ADV_CHAN_MAP_USE_CHAN_38 | HCI_ADV_CHAN_MAP_USE_CHAN_39,
                           HCI_ADV_FILTER_POL_SCAN_ALL_CONNECT_ALL))
        loge("Failed to set LE advertising params\n");

    static const uint8_t data[] = {
		2, HCI_EIR_TYPE_FLAGS, 2, /* flags - general discoverable */
		5, HCI_EIR_TYPE_FULL_NAME, 'N', 'B', '.', 't', /* name - "NB.t" */
                3, HCI_EIR_TYPE_INCOMPL_LIST_UUID16, 0x05, 0x18, /* incomplete uuid list - {time service} */
                5, HCI_EIR_FLAVE_CONN_INTS, 0x80, 0x00, 0xff, 0x00, /* relatively large range of relatively large connection intervals accepted */
    };
    if (!hciSetAdvData(data, sizeof(data), opDoneCbk, &sem))
        loge("Failed to set LE adv data\n");
    r_sem_wait(&sem);
    if(gStatus)
        loge("Failed to set adv data. Sta = %d\n", gStatus);


    logi("enabling LE adv\n");
    if (!hciSetAdvertiseEnable(true, opDoneCbk, &sem))
        loge("Failed to try to enable advertising\n");
    r_sem_wait(&sem);
    if(gStatus)
        loge("Failed to enable advertising. Sta = %d\n", gStatus);

    logi("waiting...press a key\n");
    getchar();

    stopTimeService = true;
    if (pthread_join(setTimeThread, NULL))
        loge("Failed to join the pthread of current time service.\n");
    logi("The pthread of current time service was terminated.\n");

    if (mSvcTime) {
        gattServiceStop(mSvcTime);
        gattServiceDestroy(mSvcTime);
    }
    logi("bringing down\n");
    gattBuiltinDeinit();
    gattProfileDeinit();
    attDeinit();
    l2cDeinit();
    hciDown();
    timersDeinit();
    persistStore();

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


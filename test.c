#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "gatt-builtin.h"
#include "vendorLib.h"
#include "persist.h"
#include "gattSrv.h"
#include "rfcomm.h"
#include "timer.h"
#include "l2cap.h"
#include "aapi.h"
#include "util.h"
#include "uuid.h"
#include "gatt.h"
#include "att.h"
#include "hci.h"
#include "log.h"
#include "sdp.h"
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
    sem_t *sem = (sem_t*)cbkData;

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

static void edrDiscoveredNameCbk(void *cbkData, const struct bt_addr *addr, uint8_t nameReqStatus, const char *name)
{
    struct EdrDiscDev *t = (struct EdrDiscDev*)cbkData;

    if (nameReqStatus == HCI_NAME_REQ_STATUS_COMPLETE) {
        t->haveName = true;
        strcpy(t->name, name);
        fprintf(stderr, ",");
    }
    if (nameReqStatus == HCI_NAME_REQ_STATUS_ERROR)
        fprintf(stderr, "!");
    if (nameReqStatus == HCI_NAME_REQ_STATUS_IN_PROGRESS)
        fprintf(stderr, "-");
}

static void edrDiscoveredCbk(void *cbkData, const struct bt_addr *addr, uint32_t devCls, const struct hciNameGetInfo *forNameGetting, int8_t rssi, const void *eir, uint8_t eirLen)
{
    struct EdrDiscDev **edrDevicesP = (struct EdrDiscDev**)cbkData, *t;
    bool isNew = false;

    /* see if this mac exists */
    t = *edrDevicesP;
    while (t && memcmp(t->mac, addr->addr, sizeof(t->mac)))
        t = t->next;

    if (!t) {
        t = (struct EdrDiscDev*)calloc(1, sizeof(struct EdrDiscDev));
        if (!t)
            return;

        t->next = *edrDevicesP;
        *edrDevicesP = t;

        isNew = true;
        fprintf(stderr, ".");
    }

    t->rssi = rssi;
    t->devCls = devCls;
    memcpy(t->mac, addr->addr, sizeof(t->mac));

    if (eirLen > 1)
        t->haveEir = 1;
    if (eirLen > sizeof(t->eir))
        eirLen = sizeof(t->eir);
    memcpy(t->eir, eir, eirLen);

    /* doing this at this time lowers discovery efficiency, fyi */
    if (isNew && !hciDiscoverEdrGetName(edrDiscoveredNameCbk, t, forNameGetting))
        loge("failed to request name for %02X:%02X:%02X:%02X:%02X:%02X\n", t->mac[5], t->mac[4], t->mac[3], t->mac[2], t->mac[1], t->mac[0]);
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


struct TestRfcState {
    rfc_conn_t conn;
    struct bt_addr addr;
    uint8_t dlci;
};

static void* testRfcAllocF(void *userData, const struct bt_addr *peerAddr, uint8_t dlci)
{
    struct TestRfcState *s;

    logi("RFC in from "ADDRFMT" on dlci %u\n", ADDRCONV(*peerAddr), dlci);

    s = calloc(1, sizeof(struct TestRfcState));
    if (s) {
        memcpy(&s->addr, peerAddr, sizeof(s->addr));
        s->dlci = dlci;
    }

    return s;
}

static void testRfcEvtF(void *userData, void *instance, uint8_t evt, sg data)
{
    struct TestRfcState *s = (struct TestRfcState*)instance;
    int32_t i;

    switch (evt) {
    case RFC_EVT_OPEN:
        if (sgLength(data) != sizeof(rfc_conn_t))
            loge("wrong data size to rf open event\n");
        sgSerialize(data, 0, sizeof(s->conn), &s->conn);
        sgFree(data);
        logi("RFC@["ADDRFMT",%u] open: "RFCCONNFMT"\n", ADDRCONV(s->addr), s->dlci, RFCCONNCNV(s->conn));
        break;
    case RFC_EVT_RX:
  //      logi("RFC@["ADDRFMT",%u] RX %ub\n", ADDRCONV(s->addr), s->dlci, sgLength(data));
        i = rfcTx(s->conn, data);
  //      logi("reply=%d\n", i);
        if (i != RFC_TX_RET_ENTIRE_SENT) {
            loge("TX failed - will not retry\n");
            sgFree(data);
        }
        break;
    case RFC_EVT_MAY_SEND:
        if (data)
            loge("unexpected data in may send event\n");
        logi("RFC@["ADDRFMT",%u] may send\n", ADDRCONV(s->addr), s->dlci);
        break;
    case RFC_EVT_CLOSE:
        if (data)
            loge("unexpected data in close event\n");
        logi("RFC@["ADDRFMT",%u] close\n", ADDRCONV(s->addr), s->dlci);
        free(s);
        break;
    default:
        loge("RFC@["ADDRFMT",%u] unknown event %u\n", ADDRCONV(s->addr), s->dlci, evt);
        break;
    }
}

uint16_t mGattHandleCurTime = 0;
uint16_t mGattHandleCurTimeCfg = 0;
uint16_t mGattHandleLocalTimeInfo = 0;
uint16_t mGattHandleReferenceTimeInfo = 0;

static bool testGattSrvValueReadCbk(int svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len)
{
    return false;
}

static bool testGattSrvValueWriteCbk(int svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    return false;
}

static void testGattSrvIndCbk(int svc, l2c_handle_t who, int cid, uint16_t handle, uint8_t evt, uint64_t ref)
{
    //todo
}

//sets up time service service as a sample
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
    mGattHandleLocalTimeInfo = gattServiceAddChar(svc, &uuid, GATT_PROP_READ, GATT_PERM_ALL_READ);
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
    uint8_t rfcChan;
    uint32_t sdpHandle;
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
    if (!hciUp(mac, HCI_DISP_CAP_DISP_YES_NO)) {
        loge("HCI up fail\n");
        return -1;
    }

    logi("setting name\n");
    if (!hciSetLocalName("NewBlue test", opDoneCbk, &sem))
        loge("Failed to try to set name\n");
    r_sem_wait(&sem);
    if(gStatus)
        loge("Failed to set name. Sta = %d\n", gStatus);


    logi("setting class\n");
    if (!hciSetDeviceClass(DEVICE_CLASS_SERVICE_AUDIO | DEVICE_CLASS_SERVICE_RENDERING |
		DEVICE_CLASS_SERVICE_INFORMATION | (DEVICE_CLASS_MAJOR_AV << DEVICE_CLASS_MAJOR_SHIFT) |
		(DEVICE_CLASS_MINOR_AV_PORTBL_AUDIO << DEVICE_CLASS_MINOR_AV_SHIFT), opDoneCbk, &sem))
        loge("Failed to try to set device class\n");
    r_sem_wait(&sem);
    if(gStatus)
        loge("Failed to set device class. Sta = %d\n", gStatus);


    logi("setting inquiry state\n");
    if (!hciSetDiscoverableConnectable(&vTrue, &vTrue, opDoneCbk, &sem))
        loge("Failed to try to set discoverable\n");
    r_sem_wait(&sem);
    if(gStatus)
        loge("Failed to set discoverable. Sta = %d\n", gStatus);

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

        //try EDR discovery
        {
            struct EdrDiscDev *edrDevices = NULL, *t;

            logi("Trying some EDR discovery...\n");
            h = hciDiscoverEdrStart(edrDiscoveredCbk, &edrDevices, 20000);
            if (h) {
                bool stopret;
                sleep(20);
                fprintf(stderr, "|");
                stopret = hciDiscoverEdrStop(h);
                sleep(20);
                fprintf(stderr, "\n");
                logi("discovery stop = %s\n", stopret ? "OK" : "FAIL");

                while (edrDevices) {
                    char str[128];
                    t = edrDevices;
                    edrDevices = edrDevices->next;

                    logi("Device %02X:%02X:%02X:%02X:%02X:%02X\n", t->mac[5], t->mac[4], t->mac[3], t->mac[2], t->mac[1], t->mac[0]);
                    if (t->rssi != HCI_RSSI_UNKNOWN)
                        logi("\tRSSI: %d dBm\n", t->rssi);
                    else
                        logi("\tRSSI unknown\n");
                    logi("\tCLASS: 0x%06x\n", t->devCls);
                    if (t->haveName)
                        logi("\tNAME: '%s'\n", t->name);
                    else
                        logi("\tNAME unknown\n");

                    logData(str, t->eir, 32);
                    if (t->haveEir)
                        logi("EIR: %s...\n", str);
                    else
                        logi("EIR not present\n");
                    free(t);
                }

            } else
                loge("Failed to do EDR discovery\n");
        }
    }

    l2cInit();
    sdpInit();
    rfcInit();

    rfcChan = rfcListen(RFC_CHNUM_REQ_ANY, testRfcAllocF, testRfcEvtF, NULL);
    logi("RFC CHAN: %u\n", rfcChan);

    // listen on UUID used by BluetoothChat sample program
    sdpHandle = rfcAddSdpRecord(uuid, rfcChan, "BluetoothChatInsecure");

  //  struct bt_addr addr = {{0xf3, 0xdb, 0x56, 0xf4, 0x2c, 0x40}, BT_ADDR_TYPE_EDR};
  //  logi("conn=%d\n", rfcConnect(&addr, 1, testRfcEvtF, NULL, testRfcAllocF(NULL, &addr, 3))); /* true if you shoudl expect callback(s) */

    if (!attInit())
        loge("Failed to init ATT\n");
    if (!gattProfileInit())
        loge("Failed to init GATT\n");
    if (!gattBuiltinInit())
        loge("Failed to init GATT services\n");

    svc = gattSetup();

    gattServiceStart(svc, true, false);

    gattBuiltinSetDevName("SuperLongDeviceNameThatWillProbablyNotFitIntoASinglePacketEver");
    gattBuiltinSetPreferredConnParamsWhenSlave(20, 30, 40, 400);
    gattBuiltinSetAppearance(GAP_APPEAR_GENERIC_WATCH);


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

    sdpServiceDescriptorDel(sdpHandle);
    rfcStopListen(rfcChan);

    if (svc) {
        gattServiceStop(svc);
        gattServiceDestroy(svc);
    }
    logi("bringing down\n");
    gattBuiltinDeinit();
    gattProfileDeinit();
    attDeinit();
    rfcDeinit();
    sdpDeinit();
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

void aapiPinReqCbk(const struct bt_addr *peer)
{
    const uint8_t pin[4] = {'1','2','3','4'};

    logd("aapiPinReqCbk(...)\n");
    logd("pin is '1234'\n");
    hciSecUserCbkPinCodeReq(peer, pin, sizeof(pin));
}

void aapiSspReqCbk(const struct bt_addr *peer, bt_ssp_variant_t pairing_variant, uint32_t pass_key)
{
    logd("aapiSspReqCbk(...)\n");
    if (pairing_variant == BT_SSP_VARIANT_PASSKEY_ENTRY) {
        logd("entering all zeroes since we do not support number entry for SSP\n");
        hciSecUserCbkSspUserPasskeyReq(peer, 0);
    } else if (pairing_variant == BT_SSP_VARIANT_PASSKEY_CONFIRMATION) {
        logd("confirming display of '%06u'\n", pass_key);
        hciSecUserCbkSspUserConfirmationReq(peer, true);
    } else if (pairing_variant == BT_SSP_VARIANT_PASSKEY_NOTIFICATION) {
        logd("Numeric: '%06u'\n", pass_key);
    }
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


#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "rfcomm.h"
#include "vendorLib.h"
#include "l2cap.h"
#include "hci.h"
#include "util.h"
#include "log.h"
#include "sdp.h"
#include "bt.h"

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

static void edrDiscoveredNameCbk(void *cbkData, uint8_t nameReqStatus, const char *name)
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

extern void eliza(void);

int main(int argc, char** argv)
{
    static const uint8_t mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const bool vTrue = true;
    const bool vFalse = false;
    unsigned i;
    sem_t sem;
    uniq_t h;

    fprintf(stderr, "NewBlue test\n");
    logi("LOG UP\n");

    if (sem_init(&sem, 0, 0)) {
        loge("sem init failed\n");
        return -1;
    }

    if (!hciUp(mac, true)) {
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
    sdpRegisterProfile();
    rfcRegisterProfile();

    logi("waiting...press a key\n");
    getchar();


    logi("bringing down\n");
    hciDown();

    logi("done\n");

    return 0;
}




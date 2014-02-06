#include <stdio.h>
#include <semaphore.h>
#include <unistd.h>
#include <string.h>
#include "hci.h"
#include "vendorLib.h"
#include "l2cap.h"
#include "util.h"
#include "log.h"
#include "bt.h"

struct LeDiscDev {
    struct LeDiscDev *next;
    uint8_t           mac[BT_MAC_LEN];
    bool              randomAddr;
    bool              haveRsp;
    int8_t            rssi;
    uint8_t           advType;
    uint8_t           advLen;
    uint8_t           rspLen;
    uint8_t           advData[31];
    uint8_t           rspData[31];
};


static uint8_t gStatus = 0;

static void testHciAclRxCbk(uint16_t aclConn, const void *data, uint32_t len, bool first)
{
   logi("TEST ACL RX %ub, %s\n", len, first ? "first" : "cont\n");
}

static void testHciScoRxCbk(uint16_t aclConn, const void *data, uint8_t len, uint8_t lossAmount)
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
    struct LeDiscDev **leDevicesP = (struct LeDiscDev**)cbkData;
    struct LeDiscDev *t;
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
    }

    t->rssi = rssi;
    if (replyType != HCI_ADV_TYPE_SCAN_RSP) {
        t->advType = replyType;
        memcpy(t->advData, eir, eirLen);
        t->advLen = eirLen;
    } else {
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

    if (!hciUp(mac, testHciAclRxCbk, testHciScoRxCbk, true)) {
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

    //try LE discovery
    {
        struct LeDiscDev *leDevices = NULL, *t;

        logi("Trying some LE discovery...\n");
        h = hciDiscoverLeStart(leDiscoveryCbk, &leDevices, true, false);
        if (h) {
            sleep(20);
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

    logi("waiting...press a key\n");
    getchar();


    logi("bringing down\n");
    hciDown();

    logi("done\n");

    return 0;
}

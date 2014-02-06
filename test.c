#include <stdio.h>
#include <semaphore.h>
#include "hci.h"
#include "vendorLib.h"
#include "l2cap.h"
#include "log.h"

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



#define DEVICE_CLASS_SERVICE_RENDERING		0x040000	//PRINTER, SPEAKER
#define DEVICE_CLASS_SERVICE_AUDIO		0x200000	//SPEAKER, MIC, HEADSET
#define DEVICE_CLASS_SERVICE_INFORMATION	0x800000	//WEB-server, WAP-server

#define DEVICE_CLASS_MAJOR_SHIFT		8

#define DEVICE_CLASS_MAJOR_AV			4

#define DEVICE_CLASS_MINOR_AV_SHIFT		2
#define DEVICE_CLASS_MINOR_AV_PORTBL_AUDIO	7


int main(int argc, char** argv)
{
    static const uint8_t mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const bool vTrue = true;
    const bool vFalse = false;
    sem_t sem;



    fprintf(stderr, "NewBlue test\n");
    logi("LOG UP\n");

    sem_init(&sem, 0, 0);

    if (!hciUp(mac, testHciAclRxCbk, testHciScoRxCbk)) {
        loge("HCI up fail\n");
        return -1;
    }

    logi("setting name\n");
    if (!hciSetLocalName("NewBlue test", opDoneCbk, &sem))
        loge("Failed to try to set name\n");
    sem_wait(&sem);
    if(gStatus)
        loge("Failed to set name. Sta = %d\n", gStatus);

    logi("setting class\n");
    if (!hciSetDeviceClass(DEVICE_CLASS_SERVICE_AUDIO | DEVICE_CLASS_SERVICE_RENDERING |
		DEVICE_CLASS_SERVICE_INFORMATION | (DEVICE_CLASS_MAJOR_AV << DEVICE_CLASS_MAJOR_SHIFT) |
		(DEVICE_CLASS_MINOR_AV_PORTBL_AUDIO << DEVICE_CLASS_MINOR_AV_SHIFT), opDoneCbk, &sem))
        loge("Failed to try to set device class\n");
    sem_wait(&sem);
    if(gStatus)
        loge("Failed to set device class. Sta = %d\n", gStatus);

    logi("setting inquiry state\n");
    if (!hciSetDiscoverableConnectable(&vTrue, &vTrue, opDoneCbk, &sem))
        loge("Failed to try to set discoverable\n");
    sem_wait(&sem);
    if(gStatus)
        loge("Failed to set discoverable. Sta = %d\n", gStatus);


    logi("waiting...press a key\n");
    getchar();


    logi("bringing down\n");
    hciDown();

    logi("done\n");

    return 0;
}

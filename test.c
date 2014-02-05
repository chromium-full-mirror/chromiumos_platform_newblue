#include <stdio.h>
#include "hci.h"
#include "vendorLib.h"
#include "l2cap.h"
#include "log.h"


static void testHciAclRxCbk(uint16_t aclConn, const void *data, uint32_t len, bool first)
{
   logi("TEST ACL RX %ub, %s\n", len, first ? "first" : "cont\n");
}

static void testHciScoRxCbk(uint16_t aclConn, const void *data, uint8_t len, uint8_t lossAmount)
{
   static const char *lossAmtStr[] = {"none", "some", "unknown", "all"};

   logi("TEST SCO RX %ub, loss: %s\n", len, lossAmtStr[lossAmount]);
}

int main(int argc, char** argv)
{
    static const uint8_t mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    fprintf(stderr, "NewBlue test\n");
    logi("LOG UP\n");

    if (!hciUp(mac, testHciAclRxCbk, testHciScoRxCbk)) {
        loge("HCI up fail\n");
        return -1;
    }

    logi("bringing down\n");
    hciDown();

    logi("done\n");

    return 0;
}

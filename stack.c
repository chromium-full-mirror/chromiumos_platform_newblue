#include "stack.h"
#include "aapi.h"
#include "log.h"
#include "hci.h"


/* all of these are called directly from the stack. Please please please do not block in them! */
/* these are all stubs for whenever we get real UI */


static uint8_t mKey[16];
static bool haveKey = false;

bool stackLinkKeyRequest(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, const uint8_t *key), void *cbkData) //HCI_LINK_KEY_LEN or NULL if none
{
    cbk(addr, cbkData, haveKey ? mKey : NULL);

    return true;
}

bool stackLinkKeyNotif(const struct bt_addr *addr, const uint8_t *key, uint8_t keyType) //HCI_KEY_TYPE_*
{
    logd("New link key: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
         key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7], key[8], key[9], key[10], key[11], key[12], key[13], key[14], key[15]);

    memcpy(mKey, key, sizeof(key));
    haveKey = true;

    return true;
}





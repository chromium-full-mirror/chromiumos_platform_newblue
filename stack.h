#ifndef _STACK_H_
#define _STACK_H_


#include "types.h"
#include "bt.h"


/* all of these are called directly from the stack. Please please please do not block in them! */
bool stackLinkKeyRequest(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, const uint8_t *key), void *cbkData); //HCI_LINK_KEY_LEN or NULL if none
bool stackLinkKeyNotif(const struct bt_addr *addr, const uint8_t *key, uint8_t keyType); //HCI_KEY_TYPE_*






#endif


#ifndef _STACK_H_
#define _STACK_H_


#include "types.h"
#include "bt.h"


bool stackPinCodeRequest(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, const uint8_t *pin, uint8_t len), void *cbkData); //len = 0 for "no pin" else <= HCI_PIN_MAX_LEN
bool stackLinkKeyRequest(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, const uint8_t *key), void *cbkData); //HCI_LINK_KEY_LEN
bool stackSspIoCapabilityReq(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, bool haveDisplay, bool haveKeyboard, bool haveOobData), void *cbkData);
bool stackSspUserConfirmationReq(const struct bt_addr *addr, uint32_t numeric, void (*cbk)(const struct bt_addr *addr, void *cbkData, bool confirmed), void *cbkData);
bool stackSspUserPasskeyReq(const struct bt_addr *addr, void (*doneCbk)(const struct bt_addr *addr, void *cbkData, uint32_t numeric), void *doneCbkData,void (*progressCbk)(const struct bt_addr *addr, void *cbkData, uint8_t evt), void *progressCbkData); //or STACK_NUMERIC_INVALID for "no"
bool stackSspUserOobReq(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, const uint8_t *C, const uint8_t *R), void *cbkData); //or NULLs for "no"

bool stackSspUserPasskeyNotif(const struct bt_addr *addr, uint32_t numeric);// HCI_SSP_ENTRY_*
bool stackSspRemoteEntryNotif(const struct bt_addr *addr, uint8_t evt); // HCI_SSP_ENTRY_*
bool stackSspComplete(const struct bt_addr *addr, bool success);
bool stackSspIoCapabilityNotif(const struct bt_addr *addr, bool haveDisplay, bool haveKeyboard, bool haveOobData);
bool stackLinkKeyNotif(const struct bt_addr *addr, const uint8_t *key);






#endif


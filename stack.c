#include "stack.h"
#include "log.h"
#include "hci.h"


/* all of these are called directly from the stack. Please please please do not block in them! */
/* these are all stubs for whenever we get real UI */


bool stackPinCodeRequest(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, const uint8_t *pin, uint8_t len), void *cbkData) //len = 0 for "no pin" else <= HCI_PIN_MAX_LEN
{
    const uint8_t pin[4] = {'1','2','3','4'};

    logd("pin is '1234'\n");
    cbk(addr, cbkData, pin, sizeof(pin));
    return true;
}

bool stackLinkKeyRequest(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, const uint8_t *key), void *cbkData) //HCI_LINK_KEY_LEN or NULL if none
{
    logd("no link key\n");
    return false;
}

bool stackSspIoCapabilityReq(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, uint8_t cap, bool haveOobData, uint8_t authReq), void *cbkData)
{
    logd("support display, don't care about MITM\n");

    cbk(addr, cbkData, HCI_DISP_CAP_DISP_ONLY, false, HCI_AUTH_REQMENT_NO_MITM_PROT_NO_BOND);
    return true;
}
                                                          //HCI_DISP_CAP_*, HCI_AUTH_REQMENT_* or HCI_AUTH_REQMENT_CANCEL_SSP
bool stackSspUserConfirmationReq(const struct bt_addr *addr, uint32_t numeric, void (*cbk)(const struct bt_addr *addr, void *cbkData, bool confirmed), void *cbkData)
{
    logd("confirming display of '%06u'\n", numeric);
    cbk(addr, cbkData, true);
    return true;
}

bool stackSspUserPasskeyReq(const struct bt_addr *addr, void (*doneCbk)(const struct bt_addr *addr, void *cbkData, uint32_t numeric), void *doneCbkData,
                            void (*progressCbk)(const struct bt_addr *addr, void *cbkData, uint8_t evt), void *progressCbkData) //or STACK_NUMERIC_INVALID for "no"
{
    logd("entering all zeroes since we do not support number entry for SSP\n");
    doneCbk(addr, doneCbkData, 0);
    return true;
}

bool stackSspUserOobReq(const struct bt_addr *addr, void (*cbk)(const struct bt_addr *addr, void *cbkData, const uint8_t *C, const uint8_t *R), void *cbkData) //or NULLs for "no"
{
    logd("not providing OOB\n");
    cbk(addr, cbkData, NULL, NULL);
    return true;
}

bool stackSspUserPasskeyNotif(const struct bt_addr *addr, uint32_t numeric)// HCI_SSP_ENTRY_*
{
    logd("Numeric: '%06u'\n", numeric);
    return true;
}

bool stackSspRemoteEntryNotif(const struct bt_addr *addr, uint8_t evt) // HCI_SSP_ENTRY_*
{
    logd("remote entry notif: %u\n", evt);
    return true;
}

bool stackSspComplete(const struct bt_addr *addr, bool success)
{
    logd("SSP complete: %s\n", success ? "OK" : "FAIL");
    return true;
}

bool stackSspIoCapabilityNotif(const struct bt_addr *addr, uint8_t cap, bool haveOobData, uint8_t authReq) //HCI_DISP_CAP_*, HCI_AUTH_REQMENT_*
{
    logd("Other side supports %u and has %soob and wants %u auth\n", cap, haveOobData ? "" : "no ", authReq);
    return true;
}

bool stackLinkKeyNotif(const struct bt_addr *addr, const uint8_t *key, uint8_t keyType) //HCI_KEY_TYPE_*
{
    logd("New link key: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
         key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7], key[8], key[9], key[10], key[11], key[12], key[13], key[14], key[15]);

    return true;
}





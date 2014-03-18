#ifndef _PERSIST_H_
#define _PERSIST_H_



#include "types.h"
#include "bt.h"


//key types are also bits in "haveKeys" for enumeration
#define KEY_TYPE_MITM_PROTECTED        0 /* EDR key generated with MITM protection */
#define KEY_TYPE_MITM_UNPROTECTED      1 /* EDR key generated with MITM protection */


/* callbacks are called with internal mutex held - do not call back into persist funcs or wait too long! Return true go on enumerating. False to stop. */
typedef bool (*persistKnownDevEnumeratorF)(void *cbkData, const struct bt_addr *addr, const void *name, uint32_t nameLen, uint32_t devCls, uint32_t haveKeys);
typedef bool (*persistIdentityResolutionHelperF)(void *cbkData, const struct bt_addr *addr_in, const struct bt_addr *dev_addr, const uint8_t *irk);



void persistLoad(void);  //load prefs
void persistStore(void); //store prefs

uint32_t persistGetDeviceName(void* buf); //should be at least HCI_DEV_NAME_LEN bytes long. actual length returned
bool persistSetDeviceName(const void *buf, uint32_t len); //should be at most HCI_DEV_NAME_LEN bytes long

uint8_t persistGetDiscoveryLength(void);
bool persistSetDiscoveryLength(uint8_t len);

bool persistGetAllKnownDevs(persistKnownDevEnumeratorF enumF, void *cbkData); //return false if callback terminated enumeration
bool persistGetKnownDev(const struct bt_addr *addr, void *name, uint32_t *nameLen, uint32_t *devCls);
void persistAddKnownDev(const struct bt_addr *addr, const void *name, const uint32_t *nameLen, const uint32_t *devCls);
void persistDelKnownDev(const struct bt_addr *addr);

bool persistAddDevKey(const struct bt_addr *addr, uint8_t keyType, const uint8_t *key); //addr = NULL for our own keys, all keys are HCI_LINK_KEY_LEN bytes long
bool persistDelDevKey(const struct bt_addr *addr, uint8_t keyType);                     //addr = NULL for our own keys, all keys are HCI_LINK_KEY_LEN bytes long
bool persistGetDevKey(const struct bt_addr *addr, uint8_t keyType, uint8_t *key);       //addr = NULL for our own keys, all keys are HCI_LINK_KEY_LEN bytes long

bool persistDeviceResolve(persistIdentityResolutionHelperF helpF, void *cbkData, const struct bt_addr *addr_in);         //for LE resolution


#endif


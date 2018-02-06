/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef _PERSIST_H_
#define _PERSIST_H_



#include "types.h"
#include "bt.h"


//key types are also bits in "haveKeys" for enumeration
#define KEY_TYPE_MITM_PROTECTED        0 /* EDR key generated with MITM protection */
#define KEY_TYPE_MITM_UNPROTECTED      1 /* EDR key generated without MITM protection */
#define KEY_TYPE_CSRK                  2 /* LE connection signature resolving key */
#define KEY_TYPE_IRK                   3 /* LE identity resolving key */
#define KEY_TYPE_DHK                   4 /* LE diversifier hiding key */

#define PERSIST_NUM_TYPE_SM_RANDOM     0 /* random number for SM */

/* callbacks are called with internal mutex held - do not call back into persist funcs or wait too long! Return true go on enumerating. False to stop. */
typedef bool (*persistKnownDevEnumeratorF)(void *cbkData, const struct bt_addr *addr, const void *name, uint32_t nameLen, uint32_t devCls, uint32_t haveKeys, const uint8_t *wantedKey);



void persistLoad(void);  //load prefs
void persistStore(void); //store prefs

uint32_t persistGetDeviceName(void* buf); //should be at least HCI_DEV_NAME_LEN bytes long. actual length returned
bool persistSetDeviceName(const void *buf, uint32_t len); //should be at most HCI_DEV_NAME_LEN bytes long

uint8_t persistGetDiscoveryLength(void);
bool persistSetDiscoveryLength(uint8_t len);

bool persistEnumKnownDevs(persistKnownDevEnumeratorF enumF, void *cbkData, const uint8_t *wantedKeyType); //return false if callback terminated enumeration. wantedKeyType can be null if we don't care about keys
bool persistGetKnownDev(const struct bt_addr *addr, void *name, uint32_t *nameLen, uint32_t *devCls);
void persistAddKnownDev(const struct bt_addr *addr, const void *name, const uint32_t *nameLen, bool nameIsFull, const uint32_t *devCls);
void persistDelKnownDev(const struct bt_addr *addr);

bool persistAddDevKey(const struct bt_addr *addr, uint8_t keyType, const uint8_t *key); //addr = NULL for our own keys, all keys are HCI_LINK_KEY_LEN bytes long
bool persistGetDevKey(const struct bt_addr *addr, uint8_t keyType, uint8_t *key);       //addr = NULL for our own keys, all keys are HCI_LINK_KEY_LEN bytes long
bool persistDelDevKeys(const struct bt_addr *addr);                                     //addr = NULL for our own keys, all keys are HCI_LINK_KEY_LEN bytes long

bool persistAddDevNumber(const struct bt_addr *addr, uint8_t numType, uint64_t num);    //addr = NULL for our own numbers
bool persistGetDevNumber(const struct bt_addr *addr, uint8_t numType, uint64_t *num);   //addr = NULL for our own numbers
bool persistDelDevNumbers(const struct bt_addr *addr);                                  //addr = NULL for our own numbers


#endif


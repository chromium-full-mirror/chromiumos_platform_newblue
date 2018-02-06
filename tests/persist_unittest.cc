/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <string.h>

#include <gtest/gtest.h>

#if defined(__cplusplus)
extern "C" {
#endif

#include "../hci.h"
#include "../persist.h"

#if defined(__cplusplus)
}
#endif

namespace {

class PersistTestSuite: public testing::Test {
  public:
    const struct bt_addr *getDevOneAddr() { return &devOneAddr; }
    const struct bt_addr *getDevTwoAddr() { return &devTwoAddr; }
    std::string getDevOneName() { return devOneName; }
    std::string getDevTwoName() { return devTwoName; }

  protected:
    virtual void SetUp() {
        int addrOne[BT_MAC_LEN] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        int addrTwo[BT_MAC_LEN] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        uint32_t nameOneLen = devOneName.size() + 1;
        uint32_t nameTwoLen = devTwoName.size() + 1;

        persistLoad();

        devOneName = "Name of DevOne";
        devTwoName = "Name of DevTwo";
        devOneAddr.type = BT_ADDR_TYPE_LE_RANDOM;
        memcpy(devOneAddr.addr, addrOne, sizeof(devOneAddr.addr));
        devTwoAddr.type = BT_ADDR_TYPE_LE_RANDOM;
        memcpy(devTwoAddr.addr, addrTwo, sizeof(devTwoAddr.addr));

        persistAddKnownDev(&devOneAddr, devOneName.c_str(), &nameOneLen, true, NULL);
        persistAddKnownDev(&devTwoAddr, devTwoName.c_str(), &nameTwoLen, true, NULL);
    }
    virtual void TearDown() {
        unlink(PREF_PATH);
    }

  private:
    std::string devOneName;
    std::string devTwoName;
    struct bt_addr devOneAddr;
    struct bt_addr devTwoAddr;
};

TEST_F(PersistTestSuite, LocalDeviceNameGetSet) {
    char myDevName[] = "My Dev Name";
    char myLongDevName[] = "\
        My Dev Name My Dev Name My Dev Name My Dev Name My Dev Name My Dev Name My Dev Name \
        My Dev Name My Dev Name My Dev Name My Dev Name My Dev Name My Dev Name My Dev Name \
        My Dev Name My Dev Name My Dev Name My Dev Name My Dev Name My Dev Name My Dev Name \
        My Dev Name My Dev Name";
    char name[HCI_DEV_NAME_LEN];

    // default name
    ASSERT_EQ(persistGetDeviceName(name), strlen("NB.t"));
    ASSERT_EQ(strncmp(name, "NB.t", strlen("NB.t")), 0);

    // normal name
    persistSetDeviceName(myDevName, sizeof(myDevName));
    ASSERT_EQ(persistGetDeviceName(name), sizeof(myDevName));
    ASSERT_EQ(strncmp(name, myDevName, sizeof(myDevName)), 0);

    // long name
    persistSetDeviceName(myLongDevName, sizeof(myLongDevName));
    ASSERT_EQ(persistGetDeviceName(name), HCI_DEV_NAME_LEN);
    ASSERT_EQ(strncmp(name, myLongDevName, HCI_DEV_NAME_LEN), 0);

    // no name
    persistSetDeviceName("", 0);
    ASSERT_EQ(persistGetDeviceName(name), 0);
}

TEST_F(PersistTestSuite, LocalDiscoveryGetSet) {
    // default discovery length
    ASSERT_EQ(persistGetDiscoveryLength(), 60);

    ASSERT_TRUE(persistSetDiscoveryLength(100));
    ASSERT_EQ(persistGetDiscoveryLength(), 100);
}

TEST_F(PersistTestSuite, DeviceGetAddDel) {
    char name[HCI_DEV_NAME_LEN];
    uint32_t nameLen = 0;
    uint32_t devCls = 0;

    ASSERT_TRUE(persistGetKnownDev(getDevOneAddr(), name, &nameLen, NULL));
    ASSERT_EQ(strncmp(name, getDevOneName().c_str(), nameLen), 0);

    // delete device two
    persistDelKnownDev(getDevTwoAddr());
    ASSERT_FALSE(persistGetKnownDev(getDevTwoAddr(), name, &nameLen, NULL));

    // add device one back
    nameLen = getDevTwoName().size() + 1;
    persistAddKnownDev(getDevTwoAddr(), getDevTwoName().c_str(), &nameLen, true, NULL);
    ASSERT_TRUE(persistGetKnownDev(getDevTwoAddr(), name, &nameLen, NULL));
    ASSERT_EQ(strncmp(name, getDevTwoName().c_str(), nameLen), 0);
}

TEST_F(PersistTestSuite, DeviceKeyGetAddDel) {
    uint8_t keyDHK[HCI_LE_KEY_LEN] = {0x4C, 0x68, 0x38, 0x41, 0x39, 0xF5, 0x74, 0xD8,
                                      0x36, 0xBC, 0xF3, 0x4E, 0x9D, 0xFB, 0x01, 0xBF,};
    uint8_t keyIRK[HCI_LE_KEY_LEN] = {0x57, 0x83, 0xD5, 0x21, 0x56, 0xAD, 0x6F, 0x0E,
                                      0x63, 0x88, 0x27, 0x4E, 0xC6, 0x70, 0x2E, 0xE0,};
    uint8_t key[HCI_LE_KEY_LEN] = {0,};

    // set IRK key for local device without providing key
    ASSERT_TRUE(persistGetDevKey(NULL, KEY_TYPE_IRK, key));
    ASSERT_TRUE(persistAddDevKey(NULL, KEY_TYPE_IRK, NULL));
    ASSERT_TRUE(persistGetDevKey(NULL, KEY_TYPE_IRK, key));
    ASSERT_TRUE(persistDelDevKeys(NULL));
    ASSERT_FALSE(persistGetDevKey(NULL, KEY_TYPE_IRK, key));

    // set IRK and DHK keys for device one
    ASSERT_FALSE(persistGetDevKey(getDevOneAddr(), KEY_TYPE_IRK, key));
    ASSERT_TRUE(persistAddDevKey(getDevOneAddr(), KEY_TYPE_IRK, keyIRK));
    ASSERT_TRUE(persistGetDevKey(getDevOneAddr(), KEY_TYPE_IRK, key));
    ASSERT_EQ(memcmp(keyIRK, key, HCI_LE_KEY_LEN), 0);
    ASSERT_TRUE(persistAddDevKey(getDevOneAddr(), KEY_TYPE_DHK, keyDHK));
    ASSERT_TRUE(persistGetDevKey(getDevOneAddr(), KEY_TYPE_DHK, key));
    ASSERT_EQ(memcmp(keyDHK, key, HCI_LE_KEY_LEN), 0);
    ASSERT_TRUE(persistDelDevKeys(getDevOneAddr()));
    ASSERT_FALSE(persistGetDevKey(getDevOneAddr(), KEY_TYPE_IRK, key));
    ASSERT_FALSE(persistGetDevKey(getDevOneAddr(), KEY_TYPE_DHK, key));

    // delete device two and try to add/get/delete keys of device two
    persistDelKnownDev(getDevTwoAddr());
    ASSERT_FALSE(persistGetDevKey(getDevTwoAddr(), KEY_TYPE_IRK, key));
    ASSERT_TRUE(persistDelDevKeys(getDevTwoAddr()));
    ASSERT_TRUE(persistAddDevKey(getDevTwoAddr(), KEY_TYPE_IRK, keyIRK));
}

TEST_F(PersistTestSuite, DeviceNumerGetAddDel) {
    uint64_t randNumOne = 12345678;
    uint64_t randNumTwo = 87654321;
    uint64_t num = 0;

    // set a number for local device
    ASSERT_TRUE(persistGetDevNumber(NULL, PERSIST_NUM_TYPE_SM_RANDOM, &num));
    ASSERT_TRUE(persistAddDevNumber(NULL, PERSIST_NUM_TYPE_SM_RANDOM, randNumOne));
    ASSERT_TRUE(persistGetDevNumber(NULL, PERSIST_NUM_TYPE_SM_RANDOM, &num));
    ASSERT_EQ(num, randNumOne);

    // replace the randNumOne with randNumTwo for local device
    ASSERT_TRUE(persistAddDevNumber(NULL, PERSIST_NUM_TYPE_SM_RANDOM, randNumTwo));
    ASSERT_TRUE(persistGetDevNumber(NULL, PERSIST_NUM_TYPE_SM_RANDOM, &num));
    ASSERT_EQ(num, randNumTwo);

    // delete numbers of local device
    ASSERT_TRUE(persistDelDevNumbers(NULL));
    ASSERT_FALSE(persistGetDevNumber(NULL, PERSIST_NUM_TYPE_SM_RANDOM, &num));

    // set a number for device one
    ASSERT_FALSE(persistGetDevNumber(getDevOneAddr(), PERSIST_NUM_TYPE_SM_RANDOM, &num));
    ASSERT_TRUE(persistAddDevNumber(getDevOneAddr(), PERSIST_NUM_TYPE_SM_RANDOM, randNumOne));
    ASSERT_TRUE(persistGetDevNumber(getDevOneAddr(), PERSIST_NUM_TYPE_SM_RANDOM, &num));
    ASSERT_EQ(num, randNumOne);

    // replace the randNumOne with randNumTwo for device one
    ASSERT_TRUE(persistAddDevNumber(getDevOneAddr(), PERSIST_NUM_TYPE_SM_RANDOM, randNumTwo));
    ASSERT_TRUE(persistGetDevNumber(getDevOneAddr(), PERSIST_NUM_TYPE_SM_RANDOM, &num));
    ASSERT_EQ(num, randNumTwo);

    // delete numbers of device one
    ASSERT_TRUE(persistDelDevNumbers(getDevOneAddr()));
    ASSERT_FALSE(persistGetDevNumber(getDevOneAddr(), PERSIST_NUM_TYPE_SM_RANDOM, &num));

    // delete device two and try to add/get/delete a number
    persistDelKnownDev(getDevTwoAddr());
    ASSERT_FALSE(persistGetDevNumber(getDevTwoAddr(), PERSIST_NUM_TYPE_SM_RANDOM, &num));
    ASSERT_TRUE(persistDelDevNumbers(getDevTwoAddr()));
    ASSERT_TRUE(persistAddDevNumber(getDevTwoAddr(), PERSIST_NUM_TYPE_SM_RANDOM, randNumTwo));
}

}  // namespace

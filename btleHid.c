/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <stdlib.h>
#include <string.h>
#include "btleHid.h"
#include "gatt.h"
#include "util.h"
#include "log.h"
#include "mt.h"


// FOR various strange-looking checks, before assuming they are wrong, please see https://www.bluetooth.com/specifications/gatt/viewer?attributeXmlFile=org.bluetooth.service.human_interface_device.xml

//16-bit UUIDs for HID-over-GATT
#define BTLE_UUID_HID_CHAR_BOOT_KBD_INPUT_REPORT      0x2A22  // org.bluetooth.characteristic.boot_keyboard_input_report
 //CCC MUST also be present (0x2902)
#define BTLE_UUID_HID_CHAR_BOOT_KBD_OUTPUT_REPORT     0x2A32  // org.bluetooth.characteristic.boot_keyboard_output_report
#define BTLE_UUID_HID_CHAR_BOOT_MOUSE_INPUT_REPORT    0x2A33  // org.bluetooth.characteristic.boot_mouse_input_report
 //CCC MUST also be present (0x2902)
#define BTLE_UUID_HID_CHAR_INFORMATION                0x2A4A  // org.bluetooth.characteristic.hid_information
#define BTLE_UUID_HID_CHAR_REPORT_MAP                 0x2A4B  // org.bluetooth.characteristic.report_map
 #define BTLE_UUID_HID_CHARDESCR_EXTERNAL_REPORT_REF  0x2907  // org.bluetooth.descriptor.external_report_reference
#define BTLE_UUID_HID_CHAR_CONTROL_PT                 0x2A4C  // org.bluetooth.characteristic.hid_control_point
#define BTLE_UUID_HID_CHAR_REPORT                     0x2A4D  // org.bluetooth.characteristic.report (multiples allowed)
 #define BTLE_UUID_HID_CHARDESCR_REPORT_REF           0x2908  // org.bluetooth.descriptor.report_reference
 //CCC MUST also be present (0x2902)
#define BTLE_UUID_HID_CHAR_PROTOCOL_MODE              0x2A4E  // org.bluetooth.characteristic.protocol_mode

#define BTLE_HID_REPORT_ID_INVALID                    0xFF00  // a report ID that is never valid by HID spec

// from HID spec, needed here locally for some sanity checking
#define HID_REPORT_TYPE_IN                            1
#define HID_REPORT_TYPE_OUT                           2
#define HID_REPORT_TYPE_FEATURE                       3

struct DataBuffer {
    uint32_t dataSz;
    uint8_t data[];
};

struct CharInfo {
    uint16_t charHandle;
    uint16_t valHandle;
    uint16_t endHandle;
    uint16_t cccdHandle;  //optional
    uint16_t extraHandle; //optional, only for some (BTLE_UUID_HID_CHARDESCR_EXTERNAL_REPORT_REF for BTLE_UUID_HID_CHAR_REPORT_MAP, BTLE_UUID_HID_CHARDESCR_REPORT_REF for BTLE_UUID_HID_CHAR_REPORT)
    uint8_t charProps;

    bool subbed;

    // some chars we just read once and buffer forever. For them, the data will be stored here. sometimes we update these too
    struct DataBuffer *chrData;

    // sometimes we even need to read a descriptor too. Same as above, the data will be here
    struct DataBuffer *descrData;
};

struct ReportsListNode {
    struct ReportsListNode* next;
    struct CharInfo chr;

    uint16_t reportId;  // 16 bits on purpose to allow invalid values to be set
    uint8_t reportType; // as per hid spec
};

struct HidConn {
    struct HidConn *prev;
    struct HidConn *next;

    ble_hid_conn_t id;
    gatt_client_conn_t gattConn;
    uint8_t deviceName[MAX_DEVICE_NAME_SIZE];
    uint8_t state;
    uint32_t substate; // intra-state state tracking

    uint16_t hidServiceFirstHandle;
    uint16_t hidServiceLastHandle;

    struct CharInfo bootKbdInput;
    struct CharInfo bootKbdOutput;
    struct CharInfo bootMouseInput;
    struct CharInfo hidInfo;
    struct CharInfo reportMap;
    struct CharInfo controlPt;
    struct CharInfo protocolMode;

    struct ReportsListNode *reports;
};


static BtleHidConnStateCbk mCbkState;
static BtleHidReportRxCbk mCbkRx;

static pthread_mutex_t mHidLock = PTHREAD_MUTEX_INITIALIZER;
static struct HidConn *mHidDevs = NULL;


static void btleHidAttachPerformPrerequisiteRead(gatt_client_conn_t gattConn);




/*
 * FUNCTION: hidFindConnById
 * USE:      find a HidConn struct given its id
 * PARAMS:   id - the id to find
 * RETURN:   the structure or NULL if not found
 * NOTES:    call with mHidLock held
 */
static struct HidConn *hidFindConnById(ble_hid_conn_t id)
{
    struct HidConn *c;

    for (c = mHidDevs; c && c->id != id; c = c->next);

    return c;
}

/*
 * FUNCTION: hidFindConnByGattConn
 * USE:      find a HidConn struct given a gatt connection
 * PARAMS:   gattConn - the gatt connection to find
 * RETURN:   the structure or NULL if not found
 * NOTES:    call with mHidLock held
 */
static struct HidConn *hidFindConnByGattConn(gatt_client_conn_t gattConn)
{
    struct HidConn *c;

    for (c = mHidDevs; c && c->gattConn != gattConn; c = c->next);

    return c;
}

/*
 * FUNCTION: btleHidFreeCharInfo
 * USE:      Properly free a CharInfo struct (may call into GATT to unsubscribe)
 * PARAMS:   gattConn - the CATT connection ID (for unsubscribing) - pass 0 to not do that
 *           chr - the struct
 * RETURN:   NONE
 * NOTES:    calls into gatt. watch your locks!
 */
static void btleHidFreeCharInfo(gatt_client_conn_t gattConn, struct CharInfo *chr)
{
    if (gattConn && chr->subbed && (GATT_CLI_STATUS_OK != gattClientNotifsUnsubscribe(gattConn, chr->valHandle)))
        logw("Failed to unsubscribe from notifs/inds for value handle 0x%04x\n", chr->valHandle);
    if (chr->chrData)
        free(chr->chrData);
    if (chr->descrData)
        free(chr->descrData);
}

/*
 * FUNCTION: btleHidDetachWithDev
 * USE:      Detach from a given HID device given its connection structure
 * PARAMS:   hid - the connection structure
 * RETURN:   success
 * NOTES:    call with mHidLock held
 */
static bool btleHidDetachWithDev(struct HidConn *hid)
{
    struct ReportsListNode *node = hid->reports;

    hid->state = BTLE_HID_CONN_STATE_TEARDOWN;

    btleHidFreeCharInfo(hid->gattConn, &hid->bootKbdInput);
    btleHidFreeCharInfo(hid->gattConn, &hid->bootKbdOutput);
    btleHidFreeCharInfo(hid->gattConn, &hid->bootMouseInput);
    btleHidFreeCharInfo(hid->gattConn, &hid->hidInfo);
    btleHidFreeCharInfo(hid->gattConn, &hid->reportMap);
    btleHidFreeCharInfo(hid->gattConn, &hid->controlPt);
    btleHidFreeCharInfo(hid->gattConn, &hid->protocolMode);

    while (node) {
        struct ReportsListNode *t = node;
        node = node->next;
        btleHidFreeCharInfo(hid->gattConn, &t->chr);
        free(t);
    }

    if (hid->next)
        hid->next->prev = hid->prev;

    if (hid->prev)
        hid->prev->next = hid->next;
    else
        mHidDevs = hid->next;

    free(hid);

    return true;
}

/*
 * FUNCTION: btleHidDetach
 * USE:      Detach from a HID device
 * PARAMS:   id - hid device id
 * RETURN:   the structure or NULL if not found
 * NOTES:
 */
bool btleHidDetach(ble_hid_conn_t id)
{
    struct HidConn *hid;
    bool ret = false;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnById(id);
    if (hid)
        ret = btleHidDetachWithDev(hid);
    else {
        id = 0;
        logw("Attempting to detach HID from a nonexistent HID device\n");
    }
    pthread_mutex_unlock(&mHidLock);

    if (id)
        mCbkState(id, BTLE_HID_CONN_STATE_TEARDOWN);

    return ret;
}

/*
 * FUNCTION: btleHidDetachFromGatt
 * USE:      Detach from a GATT connection
 * PARAMS:   gattConn - GATT connection
 * RETURN:   the structure or NULL if not found
 * NOTES:
 */
bool btleHidDetachFromGatt(gatt_client_conn_t gattConn)
{
    ble_hid_conn_t id = 0;
    struct HidConn *hid;
    bool ret = false;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnByGattConn(gattConn);
    if (hid) {
        ret = btleHidDetachWithDev(hid);
        id = hid->id;
    }
    else
        logw("Attempting to detach HID from an unknown GATT connection\n");
    pthread_mutex_unlock(&mHidLock);

    if (id)
        mCbkState(id, BTLE_HID_CONN_STATE_TEARDOWN);

    return ret;
}

/*
 * FUNCTION: btleHidInit
 * USE:      Init HID-over-GATT subsystem
 * PARAMS:   stateCbk - the state callback
 *           rxCbk - the data RX callback
 * RETURN:   the structure or NULL if not found
 * NOTES:    call with mHidLock held
 */
void btleHidInit(BtleHidConnStateCbk stateCbk, BtleHidReportRxCbk rxCbk)
{
    mCbkState = stateCbk;
    mCbkRx = rxCbk;
}

/*
 * FUNCTION: btleHidVerifyGattRequirementsMet
 * USE:      Now that GATT discovery is done, verify that the hid device characteristics/descriptors meet the minimum requirements of the spec
 * PARAMS:   hid - the hid device struct
 * RETURN:   the answer
 * NOTES:    called with mGattLock held.
 */
static bool btleHidVerifyGattRequirementsMet(struct HidConn *hid)
{
    struct ReportsListNode *node;

    if (hid->bootKbdInput.charHandle && !hid->bootKbdInput.cccdHandle)
        logw("HID device has a Boot Keyboard Input characteristic, but said characteristic lacks a CCCD\n");
    else if (hid->bootMouseInput.charHandle && !hid->bootMouseInput.cccdHandle)
        logw("HID device has a Boot Mouse Input characteristic, but said characteristic lacks a CCCD\n");
    else if (!hid->reportMap.charHandle)
        logw("HID device has no report map characteristic\n");
    else if (!hid->hidInfo.charHandle)
        logw("HID device has no HID info characteristic\n");
    else if (!hid->controlPt.charHandle)
        logw("HID device has no control point characteristic\n");
    else if ((hid->bootKbdInput.charHandle || hid->bootKbdOutput.charHandle || hid->bootMouseInput.charHandle) && !hid->protocolMode.charHandle)
        logw("HID device has a Boot-mode descriptor characteristics but lacks a Protocol Mode selection descriptor\n");
    else if (!hid->reports)
        logw("HID device has no reports\n");
    else if (hid->bootKbdInput.charHandle && ((hid->bootKbdInput.charProps & (GATT_PROP_BROADCAST | GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_AUTH_SIG_WRITE)) != (GATT_PROP_READ | GATT_PROP_NOTIFY)))
        loge("HID device's Boot Keyboard Input characteristic's properties are wrong (0x%04x->0x%02x)\n", hid->bootKbdInput.charHandle, hid->bootKbdInput.charProps);
    else if (hid->bootKbdOutput.charHandle && ((hid->bootKbdOutput.charProps & (GATT_PROP_BROADCAST | GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP | GATT_PROP_WRITE | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_AUTH_SIG_WRITE)) != (GATT_PROP_READ | GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RSP)))
        loge("HID device's Boot Keyboard Output characteristic's properties are wrong (0x%04x->0x%02x)\n", hid->bootKbdOutput.charHandle, hid->bootKbdOutput.charProps);
    else if (hid->bootMouseInput.charHandle && ((hid->bootMouseInput.charProps & (GATT_PROP_BROADCAST | GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_AUTH_SIG_WRITE)) != (GATT_PROP_READ | GATT_PROP_NOTIFY)))
        loge("HID device's Boot Mouse Input characteristic's properties are wrong (0x%04x->0x%02x)\n", hid->bootMouseInput.charHandle, hid->bootMouseInput.charProps);
    else if (hid->hidInfo.charHandle && ((hid->hidInfo.charProps & (GATT_PROP_BROADCAST | GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP | GATT_PROP_WRITE | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_AUTH_SIG_WRITE)) != GATT_PROP_READ))
        loge("HID device's HID info characteristic's properties are wrong (0x%04x->0x%02x)\n", hid->hidInfo.charHandle, hid->hidInfo.charProps);
    else if (hid->controlPt.charHandle && ((hid->controlPt.charProps & (GATT_PROP_BROADCAST | GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP | GATT_PROP_WRITE | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_AUTH_SIG_WRITE)) != GATT_PROP_WRITE_NO_RSP))
        loge("HID device's control point characteristic's properties are wrong (0x%04x->0x%02x)\n", hid->controlPt.charHandle, hid->controlPt.charProps);
    else if (hid->protocolMode.charHandle && ((hid->protocolMode.charProps & (GATT_PROP_BROADCAST | GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP | GATT_PROP_WRITE | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_AUTH_SIG_WRITE)) != (GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP)))
        loge("HID device's Protocol Mode selection characteristic's properties are wrong (0x%04x->0x%02x)\n", hid->protocolMode.charHandle, hid->protocolMode.charProps);
    else if (hid->reportMap.charHandle && ((hid->reportMap.charProps & (GATT_PROP_BROADCAST | GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP | GATT_PROP_WRITE | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_AUTH_SIG_WRITE)) != GATT_PROP_READ))
        loge("HID device's report map characteristic's properties are wrong (0x%04x->0x%02x)\n", hid->reportMap.charHandle, hid->reportMap.charProps);
    else {
        for (node = hid->reports; node; node = node->next) {
            if (!node->chr.charHandle)
                logw("HID report somehow has no char handle?\n");
            else if (node->chr.charProps & (GATT_PROP_BROADCAST | GATT_PROP_INDICATE | GATT_PROP_AUTH_SIG_WRITE)) // we'll check further when we know the report type
                logw("HID report's characteristic's properties are wrong (0x%04x->0x%02x)\n", node->chr.charHandle, node->chr.charProps);
            else if (!node->chr.extraHandle)
                logw("HID report has no report reference descriptor\n");
            else if ((node->chr.charProps & (GATT_PROP_NOTIFY | GATT_PROP_INDICATE)) && !node->chr.cccdHandle)
                logw("HID report lacks a CCCD while we expect it\n");
            else
                continue;
            return false;
        }
        return true;
    }

    return false;
}

/*
 * FUNCTION: btleHidGetReportListNodeByCharHandle
 * USE:      find a report node in the reports list for a given char handle in a given HID device
 * PARAMS:   hid - the hid device struct
 *           chrValueHandle - the char value handle
 * RETURN:   the list node if found, else NULL
 * NOTES:    called with mGattLock held.
 */
static struct ReportsListNode* btleHidGetReportListNodeByCharValueHandle(struct HidConn *hid, uint16_t chrValueHandle)
{
    struct ReportsListNode *node;

    for (node = hid->reports; node; node = node->next) {
        if (node->chr.valHandle == chrValueHandle)
            return node;
    }

    return NULL;
}

/*
 * FUNCTION: btleHidGetReportListNodeByReportId
 * USE:      find a report node in the reports list for a given report ID in a given HID device
 * PARAMS:   hid - the hid device struct
 *           reportId - the desired report ID
 * RETURN:   the list node if found, else NULL
 * NOTES:    called with mGattLock held.
 */
static struct ReportsListNode* btleHidGetReportListNodeByReportId(struct HidConn *hid, uint32_t reportId)
{
    struct ReportsListNode *node;

    for (node = hid->reports; node; node = node->next) {
        if (node->reportId == reportId)
            return node;
    }

    return NULL;
}

/*
 * FUNCTION: btleHidGetReportListNodeByReportIdAndType
 * USE:      find a report node in the reports list for a given report ID and a report type in a given HID device
 * PARAMS:   hid - the hid device struct
 *           reportId - the desired report ID
 *           reportType - the desired report type
 * RETURN:   the list node if found, else NULL
 * NOTES:    called with mGattLock held.
 */
static struct ReportsListNode* btleHidGetReportListNodeByReportIdAndType(struct HidConn *hid, uint16_t reportId, uint8_t reportType)
{
    struct ReportsListNode *node;

    for (node = hid->reports; node; node = node->next) {
        if (node->reportId == reportId && node->reportType == reportType)
            return node;
    }

    return NULL;
}

/*
 * FUNCTION: btleHidReportDataArrivedCbk
 * USE:      Called when a notif/ind/read reply arrives
 * PARAMS:   gattConn - the GATT client connection id
 *           chrValueHandle - the char value handle
 *           data - the data
 *           isReadReply - was this a read reply
 * RETURN:   NONE
 * NOTES:
 */
static void btleHidReportDataArrivedCbk(gatt_client_conn_t gattConn, uint16_t chrValueHandle, sg data, bool isReadReply)
{
    struct ReportsListNode* node;
    struct HidConn *hid;
    int32_t reportId;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnByGattConn(gattConn);
    if (!hid) {
        logw("No HID device found for GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (hid->state != BTLE_HID_CONN_STATE_UP) {
        logi("Dropping RXed data for connection that is not in UP state\n");
        goto out_unlock;
    }

    node = btleHidGetReportListNodeByCharValueHandle(hid, chrValueHandle);
    if (node)
        reportId = (uint32_t)node->reportId;
    else if (chrValueHandle == hid->bootKbdInput.valHandle)
        reportId = BTLE_HID_REPORT_ID_BOOT_KEYBOARD_IN;
    else if (chrValueHandle == hid->bootMouseInput.valHandle)
        reportId = BTLE_HID_REPORT_ID_BOOT_MOUSE_IN;
    else {
        logw("RXed data for value handle 0x%04x matches no report\n", chrValueHandle);
        goto out_unlock;
    }

    pthread_mutex_unlock(&mHidLock);
    mCbkRx(hid->id, reportId, data, isReadReply);
    return;

out_unlock:
    pthread_mutex_unlock(&mHidLock);
    sgFree(data);
}

/*
 * FUNCTION: btleHidAsyncDataArrivedCbk
 * USE:      Called when a notif/ind arrives
 * PARAMS:   gattConn - the GATT client connection id
 *           charValueHandle - the char value handle
 *           reliable - was this an indication
 *           data - the data
 * RETURN:   NONE
 * NOTES:
 */
static void btleHidAsyncDataArrivedCbk(gatt_client_conn_t gattConn, uint16_t charValueHandle, bool reliable, sg data)
{
    btleHidReportDataArrivedCbk(gattConn, charValueHandle, data, false);
}

/*
 * FUNCTION: btleHidCccdWriteStatusCbk
 * USE:      Called when a notif/ind subscription happens
 * PARAMS:   gattConn - the GATT client connection id
 *           valHandle - the handle of the value handle in the char whose CCCD was written
 *           forSubscribe - true if status is of subscribe, false for unsubscribe
 *           status - GATT_CLI_STATUS_*
 * RETURN:   NONE
 * NOTES:
 */
static void btleHidCccdWriteStatusCbk(gatt_client_conn_t gattConn, uint16_t valHandle, bool forSubscribe, uint8_t status)
{
    struct ReportsListNode* node;
    struct CharInfo *chr = NULL;
    ble_hid_conn_t id = 0;
    struct HidConn *hid;

    if (!forSubscribe)
        return;

    logv("CCCD subscribe status %u for val handle 0x%04x\n", status, valHandle);

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnByGattConn(gattConn);
    if (!hid) {
        logw("No HID device found for GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (status != GATT_CLI_STATUS_OK) {
        logw("a CCCD write with handle 0x%04x failed: 0x%02x - detaching!\n", valHandle, status);
        goto out_detach;
    }

    // find the character descriptor this is for
    if (valHandle == hid->bootKbdInput.valHandle)
        chr = &hid->bootKbdInput;
    else if (valHandle == hid->bootKbdOutput.valHandle)
        chr = &hid->bootKbdOutput;
    else if (valHandle == hid->bootMouseInput.valHandle)
        chr = &hid->bootMouseInput;
    else if (valHandle == hid->hidInfo.valHandle)
        chr = &hid->hidInfo;
    else if (valHandle == hid->reportMap.valHandle)
        chr = &hid->reportMap;
    else if (valHandle == hid->controlPt.valHandle)
        chr = &hid->controlPt;
    else if (valHandle == hid->protocolMode.valHandle)
        chr = &hid->protocolMode;
    else for (node = hid->reports; node && !chr; node = node->next) {
        if (valHandle == node->chr.valHandle)
            chr = &node->chr;
    }

    if (!chr) {
        logw("HID notif/ind sub for unknown characteristic!\n");
        goto out_detach;
    }

    if (chr->subbed) {
        logw("HID notif/ind sub for characteristic we're already subbed to!\n");
        goto out_detach;
    }

    chr->subbed = true;

    if (!hid->substate) {
        logw("HID notif/ind refcounting error!\n");
        goto out_detach;
    }

    if (--hid->substate)
        logv("HID: still waiting on %u CCCD writes\n", hid->substate);
    else { // WE're DONE!
        hid->state = BTLE_HID_CONN_STATE_UP;
        id = hid->id;
    }

    pthread_mutex_unlock(&mHidLock);
    if (id)
        mCbkState(id, BTLE_HID_CONN_STATE_UP);
    return;

out_detach:
    if (!btleHidDetachWithDev(hid))
        loge("Failed to detach on error in CCCDs write!\n");

out_unlock:
    pthread_mutex_unlock(&mHidLock);
}

/*
 * FUNCTION: btleHidAttachWriteCccds
 * USE:      Request the writes to the CCCDs we care about
 * PARAMS:   gattConn - the GATT client connection id
 * RETURN:   NONE
 * NOTES:    If all writes are done, proceed to the next step of initialization
 */
static void btleHidAttachWriteCccds(gatt_client_conn_t gattConn)
{
    uint16_t *arr, *charHandles, *valHandles, *cccdHandles, *reliable;
    struct ReportsListNode *node;
    struct HidConn *hid;
    uint32_t num, i;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnByGattConn(gattConn);
    if (!hid) {
        logw("No HID device found for GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (hid->state != BTLE_HID_CONN_STATE_WRITING_CCCDS) {
        logw("HID device not in WRITING CCCDs state as expected "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_detach;
    }

    // count CCCDs
    for (num = 0, node = hid->reports; node; node = node->next) {
        if (node->chr.cccdHandle)
            num++;
    }
    if (hid->bootKbdInput.cccdHandle)
        num++;
    if (hid->bootMouseInput.cccdHandle)
        num++;

    // sanity check
    if (!num) {
        logw("Unlikely that a HID device has no CCCDs for us to write\n");
        goto out_detach;
    }
    hid->substate = num;

    logd("HID: %u CCCDs to write\n", num);

    // allocate array for temporarily storing all of this, assign pointers
    arr = malloc(sizeof(uint16_t) * num * 4);
    if (!arr)
        goto out_detach;
    charHandles = arr + 0 * num;
    valHandles = arr + 1 * num;
    cccdHandles = arr + 2 * num;
    reliable = arr + 3 * num;

    // now actually write the data into the arrays
    for (i = 0, node = hid->reports; node; node = node->next) {
        if (node->chr.cccdHandle) {
            charHandles[i] = node->chr.charHandle;
            valHandles[i] = node->chr.valHandle;
            cccdHandles[i] = node->chr.cccdHandle;
            reliable[i] = !(node->chr.charProps & GATT_PROP_NOTIFY); // only opt for indicaton when notification not available
            i++;
        }
    }
    if (hid->bootKbdInput.cccdHandle) {
        charHandles[i] = hid->bootKbdInput.charHandle;
        valHandles[i] = hid->bootKbdInput.valHandle;
        cccdHandles[i] = hid->bootKbdInput.cccdHandle;
        reliable[i] = !(hid->bootKbdInput.charProps & GATT_PROP_NOTIFY);
        i++;
    }
    if (hid->bootMouseInput.cccdHandle) {
        charHandles[i] = hid->bootMouseInput.charHandle;
        valHandles[i] = hid->bootMouseInput.valHandle;
        cccdHandles[i] = hid->bootMouseInput.cccdHandle;
        reliable[i] = !(hid->bootMouseInput.charProps & GATT_PROP_NOTIFY);
        i++;
    }

    pthread_mutex_unlock(&mHidLock);
    for (i = 0; i < num; i++) {
        logv("HID: writing %s CCCD {0x%04x 0x%04x 0x%04x}\n", reliable[i] ? "reliable" : "fast", charHandles[i], valHandles[i], cccdHandles[i]);
        if (GATT_CLI_STATUS_OK != gattClientNotifsSubscribe(gattConn, charHandles[i], valHandles[i], cccdHandles[i], !!reliable[i], btleHidCccdWriteStatusCbk, btleHidAsyncDataArrivedCbk)) {
            logw("Failed to write CCCD for {0x%04x,0x%04x,0x%04x,%u\n", charHandles[i], valHandles[i], cccdHandles[i], !!reliable[i]);
            break;
        }
    }
    free(arr);

    // did it all work?
    if (i != num) {
        logw("Some CCCD writing failed! detaching\n");
        if (!btleHidDetachFromGatt(gattConn))
            loge("Failed to detach from GATT conn on error in descriptors search!\n");
        return;
    }

    // once all callbacks fired, we'll be all set to go
    return;

out_detach:
    if (!btleHidDetachWithDev(hid))
        loge("Failed to detach on error in CCCDs write!\n");

out_unlock:
    pthread_mutex_unlock(&mHidLock);
}

/*
 * FUNCTION: btleHidCheckAndProcessDescriptors
 * USE:      Now that descriptors are read, verify that the hid device is still sane
 * PARAMS:   hid - the hid device struct
 * RETURN:   the answer
 * NOTES:    called with mGattLock held.
 */
static bool btleHidCheckAndProcessDescriptors(struct HidConn *hid)
{
    struct ReportsListNode *node, *t;

    //process reports
    for (node = hid->reports; node; node = node->next) {
        uint8_t reportType, reportId;

        if (!node->chr.descrData || node->chr.descrData->dataSz != 2) {
            logw("HID report with handle 0x%04x lacks a proper report reference descriptor\n", node->chr.charHandle);
            return false;
        }
        reportId = utilGetLE8(node->chr.descrData->data + 0);
        reportType = utilGetLE8(node->chr.descrData->data + 1);

        /* Verify that the pair of report ID + report type is not a dupe.
         * Some keyboards, e.g., Logitech K780, use the same report ID
         * in both input report type and output report type.
         * Let's be tolerant and relax a little bit on checking the report ID.
         * Hence, we enforce the report ID uniqueness of the same report type
         * instead of in the same device.
         */
        t = btleHidGetReportListNodeByReportIdAndType(hid, reportId, reportType);
        if (t) {
            logw("HID report with handle 0x%04x has a report ID of 0x%02x and a report type of 0x%02x, but so does the one at 0x%04x\n", node->chr.charHandle, reportId, reportType, t->chr.charHandle);
            return false;
        }

        // verify report type, CCCDs present where needed and not present where not expected, verify capabilities
        switch (reportType) {
        case HID_REPORT_TYPE_IN:
            if ((node->chr.charProps & (GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP | GATT_PROP_AUTH_SIG_WRITE | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_BROADCAST)) != (GATT_PROP_READ | GATT_PROP_NOTIFY)) {
                logw("HID INPUT report's characteristic's properties are wrong (0x%04x->0x%02x)\n", node->chr.charHandle, node->chr.charProps);
                return false;
            }
            if (!node->chr.cccdHandle) {
                logw("HID INPUT report at 0x%04x is missing a CCCD\n", node->chr.charHandle);
                return false;
            }
            break;
        case HID_REPORT_TYPE_OUT:
            if ((node->chr.charProps & (GATT_PROP_READ | GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RSP | GATT_PROP_AUTH_SIG_WRITE | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_BROADCAST)) != (GATT_PROP_READ | GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RSP)) {
                logw("HID OUTPUT report's characteristic's properties are wrong (0x%04x->0x%02x)\n", node->chr.charHandle, node->chr.charProps);
                return false;
            }
            if (node->chr.cccdHandle) {
                logw("HID OUTPUT report at 0x%04x unexpectedly has a CCCD\n", node->chr.charHandle);
                return false;
            }
            break;
        case HID_REPORT_TYPE_FEATURE:
            if ((node->chr.charProps & (GATT_PROP_READ | GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RSP | GATT_PROP_AUTH_SIG_WRITE | GATT_PROP_NOTIFY | GATT_PROP_INDICATE | GATT_PROP_BROADCAST)) != (GATT_PROP_READ | GATT_PROP_WRITE)) {
                logw("HID FEATURE report's characteristic's properties are wrong (0x%04x->0x%02x)\n", node->chr.charHandle, node->chr.charProps);
                return false;
            }
            if (node->chr.cccdHandle) {
                logw("HID FEATURE report at 0x%04x unexpectedly has a CCCD\n", node->chr.charHandle);
                return false;
            }
            break;
        default:
            logw("HID report with handle 0x%04x has an invalid report type %u\n", node->chr.charHandle, reportType);
            return false;
        }

        //store the result now that we're sure this report makes sense
        node->reportId = reportId;
        node->reportType = reportType;
    }

    if (!hid->hidInfo.charHandle || !hid->hidInfo.chrData || hid->hidInfo.chrData->dataSz != 4) { // hid info should have data of exactly 4 bytes
        logw("HID info structure missing or invalid in size!\n");
        return false;
    }

    return true;
}

/*
 * FUNCTION: btleHidAttachPrerequisiteReadGetNext
 * USE:      figure out the next thing to read for prerequisite reads
 * PARAMS:   hid - the hid device struct
 *           handleP - if not null, store the handle to read here
 *           bufPPP - if not null, store pointer to buffer pointer here
 * RETURN:   true if there IS something next to read, false else
 * NOTES:    call with mGattLock held
 */
static bool btleHidAttachPrerequisiteReadGetNext(struct HidConn *hid, uint16_t *handleP, struct DataBuffer ***bufPPP)
{
    struct DataBuffer **bufPP = NULL;
    struct ReportsListNode *node;
    uint16_t handleToRead;

    // find first char/descr which we expect to be read and which is not yet
    if (hid->protocolMode.charHandle && !hid->protocolMode.chrData) {
        bufPP = &hid->protocolMode.chrData;
        handleToRead = hid->protocolMode.valHandle;
    }
    else if (hid->reportMap.charHandle && !hid->reportMap.chrData) {
        bufPP = &hid->reportMap.chrData;
        handleToRead = hid->reportMap.valHandle;
    }
    else if (hid->reportMap.extraHandle && !hid->reportMap.descrData) {
        bufPP = &hid->reportMap.descrData;
        handleToRead = hid->reportMap.extraHandle;
    }
    else if (hid->hidInfo.charHandle && !hid->hidInfo.chrData) {
        bufPP = &hid->hidInfo.chrData;
        handleToRead = hid->hidInfo.valHandle;
    }
    else for (node = hid->reports; node && !bufPP; node = node->next) {
        if (node->chr.extraHandle && !node->chr.descrData) {
            bufPP = &node->chr.descrData;
            handleToRead = node->chr.extraHandle;
        }
    }

    if (!bufPP)
        return false;
    if (handleP)
        *handleP = handleToRead;
    if (bufPPP)
        *bufPPP = bufPP;
    return true;
}

/*
 * FUNCTION: btleHidAttachPrerequisiteReadDataRxedCbk
 * USE:      Receive the result of a read done by btleHidAttachPerformPrerequisiteRead
 * PARAMS:   userData - our data (unused here)
 *           gattConn - the GATT client connection id
 *           trans - transaction ID (we store handle here as a way to double-check we got what we wanted)
 *           handle - handle we read
 *           error -  ATT_ERROR_NONE on success; other ATT_ERROR_* if data is NULL
 *           data - the data read or NULL on error
 * RETURN:   NONE
 * NOTES:
 */
static void btleHidAttachPrerequisiteReadDataRxedCbk(void* userData, gatt_client_conn_t gattConn, uniq_t trans, uint16_t handle, uint8_t error, sg data)
{
    uint16_t expectedHandle = 0;
    struct DataBuffer **bufPP;
    struct DataBuffer *buf;
    struct HidConn *hid;

    logv("HID: prereq read cbk read hadle 0x%04x with %sNULL ptr data\n", handle, data ? "non-" : "");

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnByGattConn(gattConn);
    if (!hid) {
        logw("No HID device found for prereq read on GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (!data) {
        logw("Failed to prereq read handle 0x%04x\n", handle);
        goto out_detach;
    }

    if (!btleHidAttachPrerequisiteReadGetNext(hid, &expectedHandle, &bufPP) || expectedHandle != handle) {
        logw("Read handle 0x%04x not same as expected (0x%04x), or none expected\n", handle, expectedHandle);
        goto out_detach;
    }

    logv("HID: prereq read matches what we expected, recording\n");

    // record this data we just read
    buf = (struct DataBuffer*)malloc(sizeof(struct DataBuffer) + sgLength(data));
    if (!buf) {
        logw("Failed to allocate buffer for prereq read data\n");
        goto out_detach;
    }
    buf->dataSz = sgLength(data);
    (void)sgSerialize(data, 0, sgLength(data), buf->data);
    *bufPP = buf;

    // go read the next thing we need to read
    pthread_mutex_unlock(&mHidLock);
    sgFree(data);
    btleHidAttachPerformPrerequisiteRead(gattConn);
    return;

out_detach:
    if (!btleHidDetachWithDev(hid))
        loge("Failed to detach on error in prereq read!\n");

out_unlock:
    pthread_mutex_unlock(&mHidLock);

    if (data)
        sgFree(data);
}

/*
 * FUNCTION: btleHidAttachPerformPrerequisiteRead
 * USE:      Request the read of the next thing we need to read before declaring the device "ready"
 * PARAMS:   gattConn - the GATT client connection id
 * RETURN:   NONE
 * NOTES:    If all reads are done, proceed to the next step of initialization
 */
static void btleHidAttachPerformPrerequisiteRead(gatt_client_conn_t gattConn)
{
    uint16_t handleToRead;
    struct HidConn *hid;

    logv("HID: prereq read do - deciding how to proceed\n");

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnByGattConn(gattConn);
    if (!hid) {
        logw("No HID device found for GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (hid->state != BTLE_HID_CONN_STATE_READING) {
        logw("HID device not in READING state as expected "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_detach;
    }

    if (!btleHidAttachPrerequisiteReadGetNext(hid, &handleToRead, NULL)) { // nothing more to read - go to next stage
        ble_hid_conn_t id;

        // again verify requirements now that we've read descriptors and such
        if (!btleHidCheckAndProcessDescriptors(hid)) {
            logw("HID device descriptor issues found. Detaching\n");
            goto out_detach;
        }

        // start writing the required things
        hid->state = BTLE_HID_CONN_STATE_WRITING_CCCDS;
        id = hid->id;
        pthread_mutex_unlock(&mHidLock);
        mCbkState(id, BTLE_HID_CONN_STATE_WRITING_CCCDS);

        btleHidAttachWriteCccds(gattConn);
        return;
    }

    logv("HID: prereq read do decided to read handle 0x%04x\n", handleToRead);

    // read the thing we've decided to read
    pthread_mutex_unlock(&mHidLock);
    if (GATT_CLI_STATUS_OK != gattClientUtilLongRead(NULL, gattConn, handleToRead, 0, (uniq_t)handleToRead, btleHidAttachPrerequisiteReadDataRxedCbk)) {
        logw("Failed to read a required handle 0x%04x\n", handleToRead);
        if (!btleHidDetachFromGatt(gattConn))
            loge("Failed to detach from GATT conn on error in descriptors search!\n");
    }
    return;


out_detach:
    if (!btleHidDetachWithDev(hid))
        loge("Failed to detach on error in descriptors search!\n");

out_unlock:
    pthread_mutex_unlock(&mHidLock);
}

/*
 * FUNCTION: btleHidUtilAddCharToCharList
 * USE:      Add a node to a characteristic list and return pointer to it
 * PARAMS:   listP - pointer to where list head is stored
 * RETURN:   char info structure for writing into or NULL on error
 * NOTES:
 */
static struct CharInfo* btleHidUtilAddCharToCharList(struct ReportsListNode **listP)
{
    struct ReportsListNode *node = calloc(1, sizeof(struct ReportsListNode));

    if (node) {
        node->next = *listP;
        *listP = node;
        node->reportId = BTLE_HID_REPORT_ID_INVALID;
        return &node->chr;
    }

    return NULL;
}

/*
 * FUNCTION: btleHidAttachAddDiscoveredChar
 * USE:      Called once per discovered characteristic
 * PARAMS:   hid - the connection structure
 *           chr - info on a character
 * RETURN:   NONE
 * NOTES:    caleld with mHidLock held
 */
static void btleHidAttachAddDiscoveredChar(struct HidConn *hid, const struct GattTraversedServiceChar *chr)
{
    bool isReport = false, isBootInputReport = false, isReportMap = false;
    struct CharInfo *dst = NULL;
    uint16_t uuid16;
    uint32_t i;

    if (!uuidToUuid16(&uuid16, &chr->uuid)) {
        logw("Unexpected uuid128-named char found in HID service: "UUIDFMT"\n", UUIDCONV(chr->uuid));
        return;
    }

    logv("HID: found char with uuid16 0x%04x\n", uuid16);
    switch (uuid16) {
    case BTLE_UUID_HID_CHAR_BOOT_KBD_INPUT_REPORT:
        dst = &hid->bootKbdInput;
        isBootInputReport = true;
        break;
    case BTLE_UUID_HID_CHAR_BOOT_KBD_OUTPUT_REPORT:
        dst = &hid->bootKbdOutput;
        break;
    case BTLE_UUID_HID_CHAR_BOOT_MOUSE_INPUT_REPORT:
        dst = &hid->bootMouseInput;
        isBootInputReport = true;
        break;
    case BTLE_UUID_HID_CHAR_INFORMATION:
        dst = &hid->hidInfo;
        break;
    case BTLE_UUID_HID_CHAR_REPORT_MAP:
        dst = &hid->reportMap;
        isReportMap = true;
        break;
    case BTLE_UUID_HID_CHAR_CONTROL_PT:
        dst = &hid->controlPt;
        break;
    case BTLE_UUID_HID_CHAR_PROTOCOL_MODE:
        dst = &hid->protocolMode;
        break;
    case BTLE_UUID_HID_CHAR_REPORT:
        dst = btleHidUtilAddCharToCharList(&hid->reports);
        isReport = true;
        break;
    default:
        logw("Unexpected char with uuid16 0x%04x found in HID service\n", uuid16);
        return;
    }

    if (!dst)
        logw("Unexpected characteristic (uuid16 0x%04x) found in HID service. Ignoring\n", uuid16);
    else if (dst->charHandle)
        logw("Unexpected duplicate characteristic (uuid16 0x%04x) found in HID. Ignoring\n", uuid16);
    else {
        logd("HID: char recorded\n");
        dst->charHandle = chr->firstHandle;
        dst->valHandle = chr->valHandle;
        dst->endHandle = chr->lastHandle;
        dst->charProps = chr->charProps;

        // handle descriptors
        for (i = 0; i < chr->numDescrs; i++) {

            if (!uuidToUuid16(&uuid16, &chr->descrs[i].uuid)) {
                logw("Unexpected uuid128-named descr found in HID service: "UUIDFMT"\n", UUIDCONV(chr->descrs[i].uuid));
                continue;
            }

            logv("HID: found descr with uuid16 0x%04x at handle 0x%04x\n", uuid16, chr->descrs[i].handle);

            //if this char belongs to a report, record it there
            if (isReport) {
                if (uuid16 == GATT_UUID_CHAR_CLI_CHAR_CFG) // cccd in a report
                    dst->cccdHandle = chr->descrs[i].handle;
                else if (uuid16 == BTLE_UUID_HID_CHARDESCR_REPORT_REF) // report ref in a report
                    dst->extraHandle = chr->descrs[i].handle;
                else
                    logi("Unexpected descriptor with uuid16 0x%04x in report at 0x%04x\n", uuid16, dst->charHandle);
            }
            else if (isBootInputReport && uuid16 == GATT_UUID_CHAR_CLI_CHAR_CFG)
                dst->cccdHandle = chr->descrs[i].handle;
            else if (isReportMap && uuid16 == BTLE_UUID_HID_CHARDESCR_EXTERNAL_REPORT_REF)
                dst->extraHandle = chr->descrs[i].handle;
            else
                logi("HID: descr in HID char is meaningless & was ignored\n");
        }
    }
}

/*
 * FUNCTION: btleHidAttachServiceTraversalCbk
 * USE:      Called by GATT when the HID service is found at request of btleHidAttach()
 * PARAMS:   userData - our data (unused here)
 *           gattConn - the GATT client connection id
 *           trans - transaction ID (unused here as we can look up by gatt conn id)
 *           data - the traversal data or NULL on error
 * RETURN:   NONE
 * NOTES:
 */
static void btleHidAttachServiceTraversalCbk(void* userData, gatt_client_conn_t gattConn, uniq_t trans, const struct GattTraversedService* data)
{
    const struct uuid hidUuid = BT_UUID_STATIC_16(BTLE_UUID_HID_SERVICE);
    struct HidConn *hid;
    ble_hid_conn_t id;
    uint32_t i;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnByGattConn(gattConn);
    if (!hid) {
        logw("Unexpected GATT service traversal callback for unknown-to-HID connection "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (!data || !uuidCmp(&data->uuid, &hidUuid)) {
        logw("Error finding or traversing HID service on GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_detach;
    }

    // copy basics
    hid->hidServiceFirstHandle = data->firstHandle;
    hid->hidServiceLastHandle = data->lastHandle;

    // record characteristics
    for (i = 0; i < data->numChars; i++)
        btleHidAttachAddDiscoveredChar(hid, &data->chars[i]);

    // verify requirements are met
    if (!btleHidVerifyGattRequirementsMet(hid)) {
        logw("HID device on GATT connection "GATTHANDLEFMT" did not meet requirements of HID-over-GATT spec and will not be usable!\n", GATTHANDLECNV(gattConn));
        goto out_detach;
    }

    //go on to read required prerequisites
    id = hid->id;
    hid->state = BTLE_HID_CONN_STATE_READING;
    pthread_mutex_unlock(&mHidLock);
    mCbkState(id, BTLE_HID_CONN_STATE_READING);
    btleHidAttachPerformPrerequisiteRead(gattConn);
    return;

out_detach:
    if (!btleHidDetachWithDev(hid))
        loge("Failed to detach on error in service search!\n");

out_unlock:
    pthread_mutex_unlock(&mHidLock);
}

/*
 * FUNCTION: btleHidAttach
 * USE:      Attach HID to a GATT connection
 * PARAMS:   gattConn - a gatt client connection
 * RETURN:   hid connection id or zero on immediate failure
 * NOTES:    expect state callback with details
 */
ble_hid_conn_t btleHidAttach(gatt_client_conn_t gattConn, const char *deviceName)
{
    const struct uuid hidUuid = BT_UUID_STATIC_16(BTLE_UUID_HID_SERVICE);
    ble_hid_conn_t hidConn = 0;
    struct HidConn *hid;
    ble_hid_conn_t id;

    hid = calloc(1, sizeof(struct HidConn));
    if (!hid)
        return 0;

    pthread_mutex_lock(&mHidLock);
    if (hidFindConnByGattConn(gattConn)) {
        logw("HID is already attached to this GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        pthread_mutex_unlock(&mHidLock);
        free(hid);
        return 0;
    }

    strncpy((char *)hid->deviceName, deviceName, MAX_DEVICE_NAME_SIZE);
    hid->id = hidConn = uniqGetNext();
    hid->gattConn = gattConn;
    hid->state = BTLE_HID_CONN_STATE_GATT_DISCOVERY;
    id = hid->id;

    hid->next = mHidDevs;
    if (mHidDevs)
        mHidDevs->prev = hid;
    mHidDevs = hid;

    pthread_mutex_unlock(&mHidLock);
    mCbkState(id, BTLE_HID_CONN_STATE_GATT_DISCOVERY);

    logi("HID attempting an attachment to GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
    if (GATT_CLI_STATUS_OK == gattClientUtilFindAndTraversePrimaryService(NULL, gattConn, &hidUuid, 0, btleHidAttachServiceTraversalCbk))
        return hidConn;

    logw("Failed to start the GATT service search\n");
    if (!btleHidDetachFromGatt(hidConn))
        loge("Failed to detach on error on attach!\n");

    return 0;
}

/*
 * FUNCTION: btleHidGetReportDescriptors
 * USE:      Get the report descriptors from a HID device
 * PARAMS:   hidId - HID device handle
 *           descriptorDataP - return data pointer here if not NULL
 *           descriptorLenP - return data length here if not NULL
 * RETURN:   true on success
 * NOTES:
 */
bool btleHidGetReportDescriptors(ble_hid_conn_t hidId, const void **descriptorDataP, uint32_t *descriptorLenP)
{
    struct HidConn *hid;
    bool ret = false;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnById(hidId);
    if (!hid)
        logw("Attempting to get HID descriptors from a nonexistent HID device\n");
    else if (hid->state != BTLE_HID_CONN_STATE_UP)
        logw("Attempting to get HID descriptors from a HID device in state %u\n", hid->state);
    else {
        if (descriptorDataP)
            *descriptorDataP = hid->reportMap.chrData->data;
        if (descriptorLenP)
            *descriptorLenP = hid->reportMap.chrData->dataSz;
        ret = true;
    }
    pthread_mutex_unlock(&mHidLock);

    return ret;
}

/*
 * FUNCTION: btleHidGetHidInfo
 * USE:      Get HID info from a HID device
 * PARAMS:   hidId - HID device handle
 *           hidVerP - hid version bcd stored here if not NULL
 *           countryP - country code stored here if not NULL
 *           flagsP - HID flags stored here if not NULL
 * RETURN:   true on success
 * NOTES:
 */
bool btleHidGetHidInfo(ble_hid_conn_t hidId, uint16_t *hidVerP, uint8_t *countryP, uint8_t *flagsP)
{
    struct HidConn *hid;
    bool ret = false;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnById(hidId);
    if (!hid)
        logw("Attempting to get HID info from a nonexistent HID device\n");
    else if (hid->state != BTLE_HID_CONN_STATE_UP)
        logw("Attempting to get HID info from a HID device in state %u\n", hid->state);
    else {
            *hidVerP = utilGetLE16(hid->hidInfo.chrData->data + 0);
        if (countryP)
            *countryP = utilGetLE8(hid->hidInfo.chrData->data + 2);
        if (flagsP)
            *flagsP = utilGetLE8(hid->hidInfo.chrData->data + 3);
        ret = true;
    }
    pthread_mutex_unlock(&mHidLock);

    return ret;
}

/*
 * FUNCTION: btleHidGetHidName
 * USE:      Get HID name from a HID device
 * PARAMS:   hidId - HID device handle
 *           hidName - hid device name stored here
 * RETURN:   true on success
 * NOTES:
 */
bool btleHidGetHidName(ble_hid_conn_t hidId, char* hidName) {
    struct HidConn* hid;
    bool ret = false;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnById(hidId);
    if (!hid)
        logw("Attempting to get HID name from a nonexistent HID device\n");
    else if (hid->state != BTLE_HID_CONN_STATE_UP)
        logw("Attempting to get HID name from a HID device in state %u\n",
             hid->state);
    else {
        if (strlen((char*) hid->deviceName))
            strncpy(hidName, (char*) hid->deviceName, MAX_DEVICE_NAME_SIZE);
        else
            strncpy(hidName, (char*) "newblue-uhid-device", MAX_DEVICE_NAME_SIZE);
        ret = true;
    }
    pthread_mutex_unlock(&mHidLock);

    return ret;
}

/*
 * FUNCTION: btleHidWriteReportWriteCbk
 * USE:      Called by GATT when our write succeeds or fails
 * PARAMS:   gattConn - a gatt client connection
 *           trans - transaction ID (unused here as we can look up by gatt conn id)
 *           handle - handle we wrote
 *           status - GATT status
 * RETURN:   NONE
 * NOTES:    it is likely that this success or failure is not actionable for us, so we do nothing
 */
static void btleHidWriteReportWriteCbk(gatt_client_conn_t gattConn, uniq_t trans, uint16_t handle, uint8_t status)
{
    if (status != GATT_CLI_STATUS_OK)
        logi("HID GATT Write to handle 0x%04x failed with status 0x%02x\n", handle, status);
}

/*
 * FUNCTION: btleHidWriteReport
 * USE:      Write to a report
 * PARAMS:   hidId - HID device handle
 *           reportId - the report we want to write
 *           data - data to write
 * RETURN:   true on success
 * NOTES:
 */
bool btleHidWriteReport(ble_hid_conn_t hidId, int32_t reportId, sg data)
{
    struct HidConn *hid;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnById(hidId);
    if (!hid)
        logw("Attempting to write to nonexistent HID device\n");
    else if (hid->state != BTLE_HID_CONN_STATE_UP)
        logw("Attempting to write to HID device in state %u\n", hid->state);
    else {
        struct ReportsListNode* node;
        uint16_t handle = 0;

        if (reportId == BTLE_HID_REPORT_ID_BOOT_KEYBOARD_IN && hid->bootKbdInput.charHandle && (hid->bootKbdInput.charProps & GATT_PROP_WRITE))
            handle = hid->bootKbdInput.valHandle;
        else if (reportId == BTLE_HID_REPORT_ID_BOOT_KEYBOARD_OUT && hid->bootKbdOutput.charHandle && (hid->bootKbdInput.charProps & GATT_PROP_WRITE))
            handle = hid->bootKbdOutput.valHandle;
        else if (reportId == BTLE_HID_REPORT_ID_BOOT_MOUSE_IN && hid->bootMouseInput.charHandle && (hid->bootKbdInput.charProps & GATT_PROP_WRITE))
            handle = hid->bootMouseInput.valHandle;
        else if (reportId >= 0 && (node = btleHidGetReportListNodeByReportIdAndType(hid, reportId, HID_REPORT_TYPE_OUT)) && (node->chr.charProps & GATT_PROP_WRITE))
            handle = node->chr.valHandle;
        else
            logw("Write to report %d is permanently impossible\n", reportId);

        if (handle) {
            gatt_client_conn_t conn = hid->gattConn;

            pthread_mutex_unlock(&mHidLock);
            return GATT_CLI_STATUS_OK == gattClientWrite(conn, handle, 0, GATT_CLI_WRITE_TYPE_WRITE, 0, data, 0, btleHidWriteReportWriteCbk);
        }
    }

    pthread_mutex_unlock(&mHidLock);
    return false;
}

/*
 * FUNCTION: btleHidReadReportReadCbk
 * USE:      Called by GATT when our read succeeds or fails
 * PARAMS:   userData - our data (unused here)
 *           gattConn - a gatt client connection
 *           trans - transaction ID (unused here as we can look up by gatt conn id)
 *           handle - handle we wrote (char value handle)
 *           status - GATT status
 *           error  - ATT_ERROR_* if status is GATT_CLI_STATUS_ERR
 *           data - the data
 * RETURN:   NONE
 * NOTES:    it is likely that this success or failure is not actionable for us, so we do nothing
 */
static void btleHidReadReportReadCbk(void* userData, gatt_client_conn_t gattConn, uniq_t trans, uint16_t handle, uint8_t status, uint8_t error, sg data)
{
    if (status != GATT_CLI_STATUS_OK || !data)
        logi("HID GATT READ to handle 0x%04x failed with status 0x%02x\n", handle, status);

    btleHidReportDataArrivedCbk(gattConn, handle, data, true);
}

/*
 * FUNCTION: btleHidReadReport
 * USE:      Read a report
 * PARAMS:   hidId - HID device handle
 *           reportId - the report we want to read
 * RETURN:   true on success
 * NOTES:
 */
bool btleHidReadReport(ble_hid_conn_t hidId, int32_t reportId)
{
    struct HidConn *hid;

    pthread_mutex_lock(&mHidLock);
    hid = hidFindConnById(hidId);
    if (!hid)
        logw("Attempting to read from nonexistent HID device\n");
    else if (hid->state != BTLE_HID_CONN_STATE_UP)
        logw("Attempting to read from HID device in state %u\n", hid->state);
    else {
        struct ReportsListNode* node;
        uint16_t handle = 0;

        if (reportId == BTLE_HID_REPORT_ID_BOOT_KEYBOARD_IN && hid->bootKbdInput.charHandle && (hid->bootKbdInput.charProps & GATT_PROP_READ))
            handle = hid->bootKbdInput.valHandle;
        else if (reportId == BTLE_HID_REPORT_ID_BOOT_KEYBOARD_OUT && hid->bootKbdOutput.charHandle && (hid->bootKbdInput.charProps & GATT_PROP_READ))
            handle = hid->bootKbdOutput.valHandle;
        else if (reportId == BTLE_HID_REPORT_ID_BOOT_MOUSE_IN && hid->bootMouseInput.charHandle && (hid->bootKbdInput.charProps & GATT_PROP_READ))
            handle = hid->bootMouseInput.valHandle;
        else if (reportId >= 0 && (node = btleHidGetReportListNodeByReportIdAndType(hid, reportId, HID_REPORT_TYPE_IN)) && (node->chr.charProps & GATT_PROP_READ))
            handle = node->chr.valHandle;
        else
            logw("Read from report %d is permanently impossible\n", reportId);

        if (handle) {
            gatt_client_conn_t conn = hid->gattConn;

            pthread_mutex_unlock(&mHidLock);
            return GATT_CLI_STATUS_OK == gattClientRead(NULL, conn, handle, 0, 0, 0, btleHidReadReportReadCbk);
        }
    }

    pthread_mutex_unlock(&mHidLock);
    return false;
}









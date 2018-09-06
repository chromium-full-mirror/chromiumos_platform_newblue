/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <stdlib.h>
#include "gattSvcMiscInfo.h"
#include "gatt.h"
#include "util.h"
#include "log.h"
#include "mt.h"



//16-bit UUID(s) for Device Info Profile
#define BTLE_UUID_MISC_INFO_SVC_CHAR_MANUF_NAME_STR        0x2A29 // org.bluetooth.characteristic.manufacturer_name_string
#define BTLE_UUID_MISC_INFO_SVC_CHAR_MODEL_NUM_STR         0x2A24 // org.bluetooth.characteristic.model_number_string
#define BTLE_UUID_MISC_INFO_SVC_CHAR_SERIAL_NUM_STR        0x2A25 // org.bluetooth.characteristic.serial_number_string
#define BTLE_UUID_MISC_INFO_SVC_CHAR_HW_REV_STR            0x2A27 // org.bluetooth.characteristic.hardware_revision_string
#define BTLE_UUID_MISC_INFO_SVC_CHAR_FW_REV_STR            0x2A26 // org.bluetooth.characteristic.firmware_revision_string
#define BTLE_UUID_MISC_INFO_SVC_CHAR_SW_REV_STR            0x2A28 // org.bluetooth.characteristic.hasoftware_revision_string
#define BTLE_UUID_MISC_INFO_SVC_CHAR_SYSTEM_ID             0x2A23 // org.bluetooth.characteristic.system_id
#define BTLE_UUID_MISC_INFO_SVC_CHAR_IEEE_CERT_LIST        0x2A2A // org.bluetooth.characteristic.ieee_11073-20601_regulatory_certification_data_list
#define BTLE_UUID_MISC_INFO_SVC_CHAR_PNP_ID                0x2A50 // org.bluetooth.characteristic.pnp_id
#define BTLE_UUID_MISC_INFO_SVC_CHAR_DEV_NAME              0x2A00 // org.bluetooth.characteristic.gap.device_name
#define BTLE_UUID_MISC_INFO_SVC_CHAR_APPEARANCE            0x2A01 // org.bluetooth.characteristic.gap.appearance


static const uint16_t mInfoTypeToUuidMap[] = {
    [GattMiscInfoTypeManufNameString] = BTLE_UUID_MISC_INFO_SVC_CHAR_MANUF_NAME_STR,
    [GattMiscInfoTypeModelNumberString] = BTLE_UUID_MISC_INFO_SVC_CHAR_MODEL_NUM_STR,
    [GattMiscInfoTypeSerialNumberString] = BTLE_UUID_MISC_INFO_SVC_CHAR_SERIAL_NUM_STR,
    [GattMiscInfoTypeHardwareRevisionString] = BTLE_UUID_MISC_INFO_SVC_CHAR_HW_REV_STR,
    [GattMiscInfoTypeFirmwareRevisionString] = BTLE_UUID_MISC_INFO_SVC_CHAR_FW_REV_STR,
    [GattMiscInfoTypeSoftwareRevisionString] = BTLE_UUID_MISC_INFO_SVC_CHAR_SW_REV_STR,
    [GattMiscInfoTypeSystemID] = BTLE_UUID_MISC_INFO_SVC_CHAR_SYSTEM_ID,
    [GattMiscInfoTypeIeee11073_20601regCertList] = BTLE_UUID_MISC_INFO_SVC_CHAR_IEEE_CERT_LIST,
    [GattMiscInfoTypePnpID] = BTLE_UUID_MISC_INFO_SVC_CHAR_PNP_ID,
    [GattMiscInfoTypeDeviceName] = BTLE_UUID_MISC_INFO_SVC_CHAR_DEV_NAME,
    [GattMiscInfoTypeAppearance] = BTLE_UUID_MISC_INFO_SVC_CHAR_APPEARANCE,
};

static const bool mInfoTypeToIsInGap[] = {
    [GattMiscInfoTypeDeviceName] = true,
    [GattMiscInfoTypeAppearance] = true,
};


struct MiscInfoSvcConn {
    struct MiscInfoSvcConn *prev;
    struct MiscInfoSvcConn *next;

    gatt_svc_misc_info_conn_t id;
    gatt_client_conn_t gattConn;
    uint8_t state;
    bool enummedGap, enummedDevInfo;

    uint16_t valHandles[GattMiscInfoEnumMax];
    sg valVals[GattMiscInfoEnumMax];
};

static GattSvcMiscInfoConnStateCbk mCbkState;
static GattSvcMiscInfoResultCbk mCbkRx;

static pthread_mutex_t mMiscInfoSvcLock = PTHREAD_MUTEX_INITIALIZER;
static struct MiscInfoSvcConn *mMiscInfoSvcDevs = NULL;



/*
 * FUNCTION: gattSvcMiscInfoTypeToUuid
 * USE:      map an info type to a UUID16
 * PARAMS:   type - the info type
 * RETURN:   uuid16 or 0 on error
 * NOTES:
 */
static uint16_t gattSvcMiscInfoTypeToUuid(enum GattSvcMiscInfoType type)
{
    return type < sizeof(mInfoTypeToUuidMap) / sizeof(*mInfoTypeToUuidMap) ? mInfoTypeToUuidMap[type] : 0;
}

/*
 * FUNCTION: gattSvcMiscInfoUuidToType
 * USE:      map a UUID16 to info type
 * PARAMS:   uuid16 - the uuid16
 * RETURN:   info type or GattMiscInfoTypeInvalid
 * NOTES:
 */
static enum GattSvcMiscInfoType gattSvcMiscInfoUuidToType(uint16_t uuid16)
{
    uint32_t i;

    for (i = 0; i < sizeof(mInfoTypeToUuidMap) / sizeof(*mInfoTypeToUuidMap); i++) {
        if (mInfoTypeToUuidMap[i] == uuid16)
            return i;
    }

    return GattMiscInfoTypeInvalid;
}

/*
 * FUNCTION: gattSvcMiscInfoIsTypeInGap
 * USE:      check if a given info type is in the GAP service (else it is in device info service)
 * PARAMS:   type - the info type
 * RETURN:   true if it is, false if it is not or we do not know of this info type
 * NOTES:
 */
static bool gattSvcMiscInfoIsTypeInGap(enum GattSvcMiscInfoType type)
{
    return type < sizeof(mInfoTypeToIsInGap) / sizeof(*mInfoTypeToIsInGap) && mInfoTypeToIsInGap[type];
}

/*
 * FUNCTION: gattSvcMiscInfoFindConnById
 * USE:      find a MiscInfoSvcConn struct given its id
 * PARAMS:   id - the id to find
 * RETURN:   the structure or NULL if not found
 * NOTES:    call with mMiscInfoSvcLock held
 */
static struct MiscInfoSvcConn *gattSvcMiscInfoFindConnById(gatt_svc_misc_info_conn_t id)
{
    struct MiscInfoSvcConn *c;

    for (c = mMiscInfoSvcDevs; c && c->id != id; c = c->next);

    return c;
}

/*
 * FUNCTION: gattSvcMiscInfoFindConnByGattConn
 * USE:      find a MiscInfoSvcConn struct given a gatt connection
 * PARAMS:   gattConn - the gatt connection to find
 * RETURN:   the structure or NULL if not found
 * NOTES:    call with mMiscInfoSvcLock held
 */
static struct MiscInfoSvcConn *gattSvcMiscInfoFindConnByGattConn(gatt_client_conn_t gattConn)
{
    struct MiscInfoSvcConn *c;

    for (c = mMiscInfoSvcDevs; c && c->gattConn != gattConn; c = c->next);

    return c;
}

/*
 * FUNCTION: gattSvcMiscInfoDetachWithDev
 * USE:      Detach from a given Misc Info Service given its connection structure
 * PARAMS:   svc - the connection structure
 * RETURN:   success
 * NOTES:    call with mMiscInfoSvcLock held
 */
static bool gattSvcMiscInfoDetachWithDev(struct MiscInfoSvcConn *svc)
{
    uint32_t i;

    if (svc->next)
        svc->next->prev = svc->prev;

    if (svc->prev)
        svc->prev->next = svc->next;
    else
        mMiscInfoSvcDevs = svc->next;

    for (i = 0; i < GattMiscInfoEnumMax; i++) {
        if (!svc->valVals[i])
            sgFree(svc->valVals[i]);
    }

    free(svc);

    return true;
}

/*
 * FUNCTION: gattSvcMiscInfoDetach
 * USE:      Detach from a Misc Info Service
 * PARAMS:   handle - MiscInfoSvc connection handle
 * RETURN:   the structure or NULL if not found
 * NOTES:
 */
bool gattSvcMiscInfoDetach(gatt_svc_misc_info_conn_t handle)
{
    struct MiscInfoSvcConn *svc;
    bool ret = false;

    pthread_mutex_lock(&mMiscInfoSvcLock);
    svc = gattSvcMiscInfoFindConnById(handle);
    if (svc)
        ret = gattSvcMiscInfoDetachWithDev(svc);
    else {
        handle = 0;
        logw("Attempting to detach Misc Info Service from a nonexistent connection\n");
    }
    pthread_mutex_unlock(&mMiscInfoSvcLock);

    if (handle)
        mCbkState(handle, BTLE_MISC_INFO_SVC_CONN_STATE_TEARDOWN);

    return ret;
}

/*
 * FUNCTION: gattSvcMiscInfoDetachFromGatt
 * USE:      Detach from a GATT connection
 * PARAMS:   gattConn - GATT connection
 * RETURN:   the structure or NULL if not found
 * NOTES:
 */
bool gattSvcMiscInfoDetachFromGatt(gatt_client_conn_t gattConn)
{
    gatt_svc_misc_info_conn_t id = 0;
    struct MiscInfoSvcConn *svc;
    bool ret = false;

    pthread_mutex_lock(&mMiscInfoSvcLock);
    svc = gattSvcMiscInfoFindConnByGattConn(gattConn);
    if (svc) {
        ret = gattSvcMiscInfoDetachWithDev(svc);
        id = svc->id;
    }
    else
        logw("Attempting to detach Misc Info Service from an unknown GATT connection\n");
    pthread_mutex_unlock(&mMiscInfoSvcLock);

    if (id)
        mCbkState(id, BTLE_MISC_INFO_SVC_CONN_STATE_TEARDOWN);

    return ret;
}

/*
 * FUNCTION: gattSvcMiscInfoReadCbk
 * USE:      Called by GATT when our read succeeds or fails
 * PARAMS:   gattConn - a gatt client connection
 *           trans - transaction ID (unused here as we can look up by gatt conn id)
 *           data - the data or NULL on error
 * RETURN:   NONE
 * NOTES:
 */
static void gattSvcMiscInfoReadCbk(gatt_client_conn_t gattConn, uniq_t trans, sg data)
{
    enum GattSvcMiscInfoType type = trans;
    struct MiscInfoSvcConn *svc;

    if (!gattSvcMiscInfoTypeToUuid(type)) {
        logw("Got a read for an invalid type\n");
        if (data)
            sgFree(data);
        return;
    }

    pthread_mutex_lock(&mMiscInfoSvcLock);
    svc = gattSvcMiscInfoFindConnByGattConn(gattConn);
    if (!svc) {
        logw("No MiscInfoSvc instance found for GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (svc->state != BTLE_MISC_INFO_SVC_CONN_STATE_UP) {
        logi("Dropping RXed data for connection that is not in UP state\n");
        goto out_unlock;
    }

    if (svc->valVals[type] && data) {
        logi("Got a duplicate read data for type %u - replacing\n", type);
        sgFree(svc->valVals[type]);
    }
    svc->valVals[type] = data;
    data = sgDup(data);
    pthread_mutex_unlock(&mMiscInfoSvcLock);
    mCbkRx(gattConn, type, data);
    return;

out_unlock:
    pthread_mutex_unlock(&mMiscInfoSvcLock);
    sgFree(data);
}

/*
 * FUNCTION: gattSvcMiscInfoAttachServiceTraversalCbk
 * USE:      Called by GATT when the Misc Info Service is found at request of gattSvcMiscInfoAttach()
 * PARAMS:   gattConn - the GATT client connection id
 *           trans - transaction ID (unused here as we can look up by MiscInfoSvc conn handle)
 *           data - the traversal data or NULL on error
 * RETURN:   NONE
 * NOTES:
 */
static void gattSvcMiscInfoAttachServiceTraversalCbk(gatt_client_conn_t gattConn, uniq_t trans, const struct GattTraversedService* data)
{
    const struct uuid devInfoSvcUuid = BT_UUID_STATIC_16(BTLE_UUID_DEVICE_INFO_SERVICE);
    const struct uuid gapSvcUuid = BT_UUID_STATIC_16(BTLE_UUID_GAP_SERVICE);
    gatt_svc_misc_info_conn_t id;
    struct MiscInfoSvcConn *svc;
    bool isGap = false;
    uint32_t i;

    pthread_mutex_lock(&mMiscInfoSvcLock);
    svc = gattSvcMiscInfoFindConnByGattConn(gattConn);
    if (!svc) {
        logw("Unexpected GATT service traversal callback for unknown-to-MiscInfoSvc connection "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (svc->state != BTLE_MISC_INFO_SVC_CONN_STATE_GATT_DISCOVERY) {
        logw("Unxpected state %u when getting GATT discovery results for MiscInfoSvc\n", svc->state);
        goto out_detach;
    }

    if (trans == BTLE_UUID_DEVICE_INFO_SERVICE) {
        if (svc->enummedDevInfo) {
            logw("Unxpected duplicate DevInfo svc enum when getting GATT discovery results for MiscInfoSvc\n");
            goto out_detach;
        }
        svc->enummedDevInfo = true;
        if (!data || !uuidCmp(&data->uuid, &devInfoSvcUuid)) {
            logi("Device has no DeviceInfo service\n");
            goto out_unlock;
        }
    }
    else if (trans == BTLE_UUID_GAP_SERVICE) {
        if (svc->enummedGap) {
            logw("Unxpected duplicate GAP svc enum when getting GATT discovery results for MiscInfoSvc\n");
            goto out_detach;
        }
        svc->enummedGap = true;
        isGap = true;
        if (!data || !uuidCmp(&data->uuid, &gapSvcUuid)) {
            logi("Device has no GAP service - this is spec violation!\n");
            goto out_unlock;
        }
    }
    else {
        logw("Unxpected trans ID when getting GATT discovery results for MiscInfoSvc\n");
        goto out_detach;
    }

    // record characteristics we care about
    for (i = 0; i < data->numChars; i++) {
        enum GattSvcMiscInfoType type;
        uint16_t uuid16;

        if (!uuidToUuid16(&uuid16, &data->chars[i].uuid)) { // all the UUIDs we care for are UUID16s
            logd("Ignoring attr with uuid128 "UUIDFMT"\n", UUIDCONV(data->chars[i].uuid));
            continue;
        }

        type = gattSvcMiscInfoUuidToType(uuid16);
        if (type == GattMiscInfoTypeInvalid) {
            logd("Ignoring attr with uninteresting UUID 0x%04x\n", uuid16);
            continue;
        }

        logd("Mapping uuid 0x%04x to info type %u\n", uuid16, type);
        if ((gattSvcMiscInfoIsTypeInGap(type) && !isGap) || (!gattSvcMiscInfoIsTypeInGap(type) && isGap)) {
            logw("UUID 0x%04x found %s GAP, while expected %s GAP - ignoring\n", uuid16, isGap ? "in" : "out of", isGap ? "out of" : "in");
            continue;
        }

        if (svc->valHandles[type]) {
            logw("Ignoring duplicate attr UUID 0x%04x (handles 0x%04x and 0x%04x)\n", uuid16, svc->valHandles[type], data->chars[i].valHandle);
            continue;
        }

        svc->valHandles[type] = data->chars[i].valHandle;
    }

    // see if we're ready to go to UP state (all enums done), else just out and wait smoe more
    if (!svc->enummedDevInfo || !svc->enummedGap)
        goto out_unlock;

    // go to UP stage
    id = svc->id;
    svc->state = BTLE_MISC_INFO_SVC_CONN_STATE_UP;
    pthread_mutex_unlock(&mMiscInfoSvcLock);
    mCbkState(id, BTLE_MISC_INFO_SVC_CONN_STATE_UP);
    return;

out_detach:
    if (!gattSvcMiscInfoDetachWithDev(svc))
        loge("Failed to detach on error in service search!\n");

out_unlock:
    pthread_mutex_unlock(&mMiscInfoSvcLock);
}

/*
 * FUNCTION: gattSvcMiscInfoAttach
 * USE:      Attach Misc Info Service to a GATT connection
 * PARAMS:   gattConn - a gatt client connection
 * RETURN:   MiscInfoSvc connection handle or zero on immediate failure
 * NOTES:    expect state callback with details
 */
gatt_svc_misc_info_conn_t gattSvcMiscInfoAttach(gatt_client_conn_t gattConn)
{
    const struct uuid devInfoSvcUuid = BT_UUID_STATIC_16(BTLE_UUID_DEVICE_INFO_SERVICE);
    const struct uuid gapSvcUuid = BT_UUID_STATIC_16(BTLE_UUID_GAP_SERVICE);
    gatt_svc_misc_info_conn_t svcConn = 0;
    struct MiscInfoSvcConn *svc;

    svc = calloc(1, sizeof(struct MiscInfoSvcConn));
    if (!svc)
        return 0;

    pthread_mutex_lock(&mMiscInfoSvcLock);
    if (gattSvcMiscInfoFindConnByGattConn(gattConn)) {
        logw("Misc Info Service is already attached to this GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        pthread_mutex_unlock(&mMiscInfoSvcLock);
        free(svc);
        return 0;
    }

    svc->id = svcConn = uniqGetNext();
    svc->gattConn = gattConn;
    svc->state = BTLE_MISC_INFO_SVC_CONN_STATE_GATT_DISCOVERY;

    svc->next = mMiscInfoSvcDevs;
    if (mMiscInfoSvcDevs)
        mMiscInfoSvcDevs->prev = svc;
    mMiscInfoSvcDevs = svc;

    pthread_mutex_unlock(&mMiscInfoSvcLock);
    mCbkState(svcConn, BTLE_MISC_INFO_SVC_CONN_STATE_GATT_DISCOVERY);

    logi("Misc Info Service attempting an attachment to GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
    if (GATT_CLI_STATUS_OK != gattClientUtilFindAndTraversePrimaryService(gattConn, &devInfoSvcUuid, (uniq_t)BTLE_UUID_DEVICE_INFO_SERVICE, gattSvcMiscInfoAttachServiceTraversalCbk))
        goto fail;

    if (GATT_CLI_STATUS_OK != gattClientUtilFindAndTraversePrimaryService(gattConn, &gapSvcUuid, (uniq_t)BTLE_UUID_GAP_SERVICE, gattSvcMiscInfoAttachServiceTraversalCbk))
        goto fail;

    // no failure so far...
    return svcConn;

fail:
    logw("Failed to start the GATT service search\n");
    if (!gattSvcMiscInfoDetachFromGatt(svcConn))
        loge("Failed to detach on error on attach!\n");

    return 0;
}

/*
 * FUNCTION: gattSvcMiscInfoRequestRead
 * USE:      Request an info reading
 * PARAMS:   handle - MiscInfoSvc connection handle
 *           type - info type to read
 * RETURN:   false on immediate error, else wait for GattSvcMiscInfoResultCbk() cbk
 * NOTES:    cbk MAY BE CALLED EVEN BEFORE THIS FUNC RETURNS!
 */
bool gattSvcMiscInfoRequestRead(gatt_svc_misc_info_conn_t handle, enum GattSvcMiscInfoType type)
{
    struct MiscInfoSvcConn *svc;

    if (!gattSvcMiscInfoTypeToUuid(type)) {
        logw("Attempting to get MiscInfoSvc into type that is not known: %u\n", type);
        return false;
    }

    pthread_mutex_lock(&mMiscInfoSvcLock);
    svc = gattSvcMiscInfoFindConnById(handle);
    if (!svc)
        logw("Attempting to get MiscInfoSvc level from a nonexistent MiscInfoSvc instance\n");
    else if (svc->state != BTLE_MISC_INFO_SVC_CONN_STATE_UP)
        logw("Attempting to get MiscInfoSvc level from a MiscInfoSvc instance in state %u\n", svc->state);
    else {
        gatt_client_conn_t conn = svc->gattConn;
        uint16_t handle = svc->valHandles[type];
        sg val = svc->valVals[type];

        logd("MiscInfo: requested type %u. Handle=0x%04x, valCache=%p\n", type, handle, val);

        if (val)
            val = sgDup(val);

        pthread_mutex_unlock(&mMiscInfoSvcLock);

        if (!handle) // no such attribute - tell caller right away
            return false;
        else if (!val) // no cache? schedule a read
            return GATT_CLI_STATUS_OK == gattClientUtilLongRead(conn, handle, 0, type, gattSvcMiscInfoReadCbk);
        else {
            mCbkRx(handle, type, val);
            return true;
        }
    }
    pthread_mutex_unlock(&mMiscInfoSvcLock);

    return false;
}

/*
 * FUNCTION: gattSvcMiscInfoInit
 * USE:      Init MiscInfoSvc subsystem
 * PARAMS:   stateCbk - the state callback
 *           rxCbk - the data RX callback
 * RETURN:   the structure or NULL if not found
 * NOTES:    call with mHidLock held
 */
void gattSvcMiscInfoInit(GattSvcMiscInfoConnStateCbk stateCbk, GattSvcMiscInfoResultCbk rxCbk)
{
    mCbkState = stateCbk;
    mCbkRx = rxCbk;
}


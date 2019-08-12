/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <stdlib.h>
#include "gattSvcBattery.h"
#include "gatt.h"
#include "util.h"
#include "log.h"
#include "mt.h"


// For details, please see https://www.bluetooth.com/specifications/gatt/viewer?attributeXmlFile=org.bluetooth.service.battery_service.xml


//16-bit UUID(s) for Battery Service
#define BTLE_UUID_BATT_SVC_CHAR_BATTERY_LEVEL       0x2A19  // org.bluetooth.characteristic.battery_level
 //CCC MAY be present (0x2902)



struct BattSvcConn {
    struct BattSvcConn *prev;
    struct BattSvcConn *next;

    gatt_svc_batt_conn_t id;
    gatt_client_conn_t gattConn;
    uint8_t state;

    uint16_t charStartHandle, charValHandle, charCccdHandle;
};

static GattSvcBattConnStateCbk mCbkState;
static GattSvcBattUpdateCbk mCbkRx;

static pthread_mutex_t mBattSvcLock = PTHREAD_MUTEX_INITIALIZER;
static struct BattSvcConn *mBattSvcDevs = NULL;





/*
 * FUNCTION: gattSvcBattFindConnById
 * USE:      find a BattSvcConn struct given its id
 * PARAMS:   id - the id to find
 * RETURN:   the structure or NULL if not found
 * NOTES:    call with mBattSvcLock held
 */
static struct BattSvcConn *gattSvcBattFindConnById(gatt_svc_batt_conn_t id)
{
    struct BattSvcConn *c;

    for (c = mBattSvcDevs; c && c->id != id; c = c->next);

    return c;
}

/*
 * FUNCTION: gattSvcBattFindConnByGattConn
 * USE:      find a BattSvcConn struct given a gatt connection
 * PARAMS:   gattConn - the gatt connection to find
 * RETURN:   the structure or NULL if not found
 * NOTES:    call with mBattSvcLock held
 */
static struct BattSvcConn *gattSvcBattFindConnByGattConn(gatt_client_conn_t gattConn)
{
    struct BattSvcConn *c;

    for (c = mBattSvcDevs; c && c->gattConn != gattConn; c = c->next);

    return c;
}

/*
 * FUNCTION: gattSvcBattDetachWithDev
 * USE:      Detach from a given Battery Service given its connection structure
 * PARAMS:   svc - the connection structure
 * RETURN:   success
 * NOTES:    call with mBattSvcLock held
 */
static bool gattSvcBattDetachWithDev(struct BattSvcConn *svc)
{
    if (svc->next)
        svc->next->prev = svc->prev;

    if (svc->prev)
        svc->prev->next = svc->next;
    else
        mBattSvcDevs = svc->next;

    free(svc);

    return true;
}

/*
 * FUNCTION: gattSvcBattDetach
 * USE:      Detach from a Battery Service
 * PARAMS:   handle - BattSvc connection handle
 * RETURN:   the structure or NULL if not found
 * NOTES:
 */
bool gattSvcBattDetach(gatt_svc_batt_conn_t handle)
{
    struct BattSvcConn *svc;
    bool ret = false;

    pthread_mutex_lock(&mBattSvcLock);
    svc = gattSvcBattFindConnById(handle);
    if (svc)
        ret = gattSvcBattDetachWithDev(svc);
    else {
        handle = 0;
        logw("Attempting to detach Battery Service from a nonexistent connection\n");
    }
    pthread_mutex_unlock(&mBattSvcLock);

    if (handle)
        mCbkState(handle, BTLE_BATT_SVC_CONN_STATE_TEARDOWN);

    return ret;
}

/*
 * FUNCTION: gattSvcBattDetachFromGatt
 * USE:      Detach from a GATT connection
 * PARAMS:   gattConn - GATT connection
 * RETURN:   the structure or NULL if not found
 * NOTES:
 */
bool gattSvcBattDetachFromGatt(gatt_client_conn_t gattConn)
{
    gatt_svc_batt_conn_t id = 0;
    struct BattSvcConn *svc;
    bool ret = false;

    pthread_mutex_lock(&mBattSvcLock);
    svc = gattSvcBattFindConnByGattConn(gattConn);
    if (svc) {
        ret = gattSvcBattDetachWithDev(svc);
        id = svc->id;
    }
    else
        logw("Attempting to detach Battery Service from an unknown GATT connection\n");
    pthread_mutex_unlock(&mBattSvcLock);

    if (id)
        mCbkState(id, BTLE_BATT_SVC_CONN_STATE_TEARDOWN);

    return ret;
}

/*
 * FUNCTION: gattSvcBattDataArrivedCbk
 * USE:      Called when a notif/ind/read reply arrives
 * PARAMS:   gattConn - the GATT client connection id
 *           charValueHandle - the char value handle
 *           data - the data
 *           isReadReply - was this a read reply
 * RETURN:   NONE
 * NOTES:
 */
static void gattSvcBattDataArrivedCbk(gatt_client_conn_t gattConn, uint16_t charValueHandle, sg data, bool isReadReply)
{
    struct BattSvcConn *svc;
    uint8_t percent = 255;

    pthread_mutex_lock(&mBattSvcLock);
    svc = gattSvcBattFindConnByGattConn(gattConn);
    if (!svc) {
        logw("No BattSvc instance found for GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (svc->state != BTLE_BATT_SVC_CONN_STATE_UP) {
        logi("Dropping RXed data for connection that is not in UP state\n");
        goto out_unlock;
    }

    if (charValueHandle != svc->charValHandle) {
        logi("Dropping RXed data for unknown handle 0x%04x\n", charValueHandle);
        goto out_unlock;
    }

    if (sgLength(data) != 1 || !sgSerializeCutFront(data, &percent, sizeof(percent)) || percent > 100)  {
        logi("Dropping RXed data as its length or data are invalid: %u, %u\n", (unsigned)sgLength(data), percent);
        goto out_unlock;
    }

    sgFree(data);
    pthread_mutex_unlock(&mBattSvcLock);
    mCbkRx(gattConn, percent, isReadReply);
    return;

out_unlock:
    pthread_mutex_unlock(&mBattSvcLock);
    sgFree(data);
}

/*
 * FUNCTION: gattSvcBattAsyncDataArrivedCbk
 * USE:      Called when a notif/ind arrives
 * PARAMS:   gattConn - the GATT client connection id
 *           charValueHandle - the char value handle
 *           reliable - was this an indication
 *           data - the data
 * RETURN:   NONE
 * NOTES:
 */
static void gattSvcBattAsyncDataArrivedCbk(gatt_client_conn_t gattConn, uint16_t charValueHandle, bool reliable, sg data)
{
    gattSvcBattDataArrivedCbk(gattConn, charValueHandle, data, false);
}

/*
 * FUNCTION: gattSvcBattReadReportReadCbk
 * USE:      Called by GATT when our read succeeds or fails
 * PARAMS:   userData - our data (unused here)
 *           gattConn - a gatt client connection
 *           trans - transaction ID (unused here as we can look up by gatt conn id)
 *           handle - handle we read (char's value handle)
 *           status - GATT status
 *           error - ATT_ERROR_* if status is GATT_CLI_STATUS_ERR
 *           data - the data
 * RETURN:   NONE
 * NOTES:    it is likely that this success or failure is not actionable for us, so we do nothing
 */
static void gattSvcBattReadReportReadCbk(void* userData, gatt_client_conn_t gattConn, uniq_t trans, uint16_t handle, uint8_t status, uint8_t error, sg data)
{
    if (status != GATT_CLI_STATUS_OK || !data)
        logi("BattSvc GATT READ to handle 0x%04x failed with status 0x%02x\n", handle, status);

    gattSvcBattDataArrivedCbk(gattConn, handle, data, true);
}

/*
 * FUNCTION: gattSvcBattCccdWriteStatusCbk
 * USE:      Called when a notif/ind subscription happens
 * PARAMS:   gattConn - the GATT client connection id
 *           valHandle - the handle of the value handle in the char whose CCCD was written
 *           forSubscribe - true if status is of subscribe, false for unsubscribe
 *           status - GATT_CLI_STATUS_*
 * RETURN:   NONE
 * NOTES:
 */
static void gattSvcBattCccdWriteStatusCbk(gatt_client_conn_t gattConn, uint16_t valHandle, bool forSubscribe, uint8_t status)
{
    gatt_svc_batt_conn_t id = 0;
    struct BattSvcConn *svc;

    if (!forSubscribe)
        return;

    logd("CCCD subscribe status %u for val handle 0x%04x\n", status, valHandle);

    pthread_mutex_lock(&mBattSvcLock);
    svc = gattSvcBattFindConnByGattConn(gattConn);
    if (!svc) {
        logw("No BattSvc instance found for GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (svc->state != BTLE_BATT_SVC_CONN_STATE_WRITING_CCCDS) {
        logw("Unxpected state %u when getting GATT CCCD write results for BattSvc\n", svc->state);
        goto out_detach;
    }

    if (status != GATT_CLI_STATUS_OK) {
        logw("a CCCD write with handle 0x%04x failed: 0x%02x - detaching!\n", valHandle, status);
        goto out_detach;
    }

    // we're up now
    svc->state = BTLE_BATT_SVC_CONN_STATE_UP;
    pthread_mutex_unlock(&mBattSvcLock);
    mCbkState(id, BTLE_BATT_SVC_CONN_STATE_UP);
    return;

out_detach:
    if (!gattSvcBattDetachWithDev(svc))
        loge("Failed to detach on error in service search!\n");

out_unlock:
    pthread_mutex_unlock(&mBattSvcLock);
}

/*
 * FUNCTION: gattSvcBatteryAttachServiceTraversalCbk
 * USE:      Called by GATT when the Battery Service is found at request of gattSvcBattAttach()
 * PARAMS:   userData - our data (unused here)
 *           gattConn - the GATT client connection id
 *           trans - transaction ID (unused here as we can look up by BattSvc conn handle)
 *           data - the traversal data or NULL on error
 * RETURN:   NONE
 * NOTES:
 */
static void gattSvcBatteryAttachServiceTraversalCbk(void* userData, gatt_client_conn_t gattConn, uniq_t trans, const struct GattTraversedService* data)
{
    const struct uuid battSvcUuid = BT_UUID_STATIC_16(BTLE_UUID_BATTERY_SERVICE);
    struct BattSvcConn *svc;
    gatt_svc_batt_conn_t id;
    uint32_t i, j;

    pthread_mutex_lock(&mBattSvcLock);
    svc = gattSvcBattFindConnByGattConn(gattConn);
    if (!svc) {
        logw("Unexpected GATT service traversal callback for unknown-to-BattSvc connection "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_unlock;
    }

    if (svc->state != BTLE_BATT_SVC_CONN_STATE_GATT_DISCOVERY) {
        logw("Unxpected state %u when getting GATT discovery results for BattSvc\n", svc->state);
        goto out_detach;
    }

    if (!data || !uuidCmp(&data->uuid, &battSvcUuid)) {
        logw("Error finding or traversing BattSvc service on GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        goto out_detach;
    }

    // verify the only characteristic we care about exists, and see if it has a CCCD
    for (i = 0; i < data->numChars; i++) {
        uint16_t uuid16;

        if (!uuidToUuid16(&uuid16, &data->chars[i].uuid) || uuid16 != BTLE_UUID_BATT_SVC_CHAR_BATTERY_LEVEL) {
            logi("Ignoring unrecognized characteristic "UUIDFMT" in BattSvc\n", UUIDCONV(data->chars[i].uuid));
            continue;
        }
        if (svc->charStartHandle) {
            logw("BattSvc has more than one BatteryValue characteristics: 0x%04x and 0x%04x\n", svc->charStartHandle, data->chars[i].firstHandle);
            goto out_detach;
        }
        if ((data->chars[i].charProps & (GATT_PROP_BROADCAST | GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP | GATT_PROP_WRITE | GATT_PROP_INDICATE | GATT_PROP_AUTH_SIG_WRITE)) != GATT_PROP_READ) {
            logw("BattSvc's BatteryValue characteristic has invalid props: 0x%02x\n", data->chars[i].charProps);
            goto out_detach;
        }

        svc->charStartHandle = data->chars[i].firstHandle;
        svc->charValHandle = data->chars[i].valHandle;

        for (j = 0; j < data->chars[i].numDescrs; j++) {
            if (!uuidToUuid16(&uuid16, &data->chars[i].descrs[j].uuid) || uuid16 != GATT_UUID_CHAR_CLI_CHAR_CFG) {
                logi("Ignoring unrecognized descriptor "UUIDFMT" in BattSvc's BatteryValue characteristic\n", UUIDCONV(data->chars[i].descrs[j].uuid));
                continue;
            }
            if (svc->charCccdHandle) {
                logw("BattSvc's BatteryValue characteristic has more than one CCCD: 0x%04x and 0x%04x\n", svc->charCccdHandle, data->chars[i].descrs[j].handle);
                goto out_detach;
            }
            svc->charCccdHandle = data->chars[i].descrs[j].handle;
        }

        if ((svc->charCccdHandle && !(data->chars[i].charProps & GATT_PROP_NOTIFY)) || (!svc->charCccdHandle && (data->chars[i].charProps & GATT_PROP_NOTIFY))) {
            logw("BattSvc's BatteryValue 's CCCD existence disagrees with props: 0x%04x 0x%02x\n", svc->charCccdHandle, data->chars[i].charProps);
            goto out_detach;
        }
    }

    if (!svc->charStartHandle) {
        logw("BattSvc failed to find BatteryValue characteristic!\n");
        goto out_detach;
    }

    // if we have a CCCD, go to CCCD write stage, else just go to UP stage
    id = svc->id;
    if (svc->charCccdHandle) {
        uint16_t charHandle = svc->charStartHandle, valHandle = data->chars[i].valHandle, cccdHandle = svc->charCccdHandle;
        svc->state = BTLE_BATT_SVC_CONN_STATE_WRITING_CCCDS;
        pthread_mutex_unlock(&mBattSvcLock);
        mCbkState(id, BTLE_BATT_SVC_CONN_STATE_WRITING_CCCDS);
        if (GATT_CLI_STATUS_OK != gattClientNotifsSubscribe(gattConn, charHandle, valHandle, cccdHandle, false, gattSvcBattCccdWriteStatusCbk, gattSvcBattAsyncDataArrivedCbk)) {
            logw("Failed to write CCCD for {0x%04x,0x%04x,0x%04x)\n", charHandle, valHandle, cccdHandle);
            if (!gattSvcBattDetachFromGatt(gattConn))
                loge("Failed to detach from GATT conn on error in CCCD write!\n");
        }
    }
    else {
        svc->state = BTLE_BATT_SVC_CONN_STATE_UP;
        pthread_mutex_unlock(&mBattSvcLock);
        mCbkState(id, BTLE_BATT_SVC_CONN_STATE_UP);
    }
    return;

out_detach:
    if (!gattSvcBattDetachWithDev(svc))
        loge("Failed to detach on error in service search!\n");

out_unlock:
    pthread_mutex_unlock(&mBattSvcLock);
}

/*
 * FUNCTION: gattSvcBattAttach
 * USE:      Attach Battery Service to a GATT connection
 * PARAMS:   gattConn - a gatt client connection
 * RETURN:   BattSvc connection handle or zero on immediate failure
 * NOTES:    expect state callback with details
 */
gatt_svc_batt_conn_t gattSvcBattAttach(gatt_client_conn_t gattConn)
{
    const struct uuid battSvcUuid = BT_UUID_STATIC_16(BTLE_UUID_BATTERY_SERVICE);
    gatt_svc_batt_conn_t svcConn = 0;
    struct BattSvcConn *svc;

    svc = calloc(1, sizeof(struct BattSvcConn));
    if (!svc)
        return 0;

    pthread_mutex_lock(&mBattSvcLock);
    if (gattSvcBattFindConnByGattConn(gattConn)) {
        logw("Battery Service is already attached to this GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
        pthread_mutex_unlock(&mBattSvcLock);
        free(svc);
        return 0;
    }

    svc->id = svcConn = uniqGetNext();
    svc->gattConn = gattConn;
    svc->state = BTLE_BATT_SVC_CONN_STATE_GATT_DISCOVERY;

    svc->next = mBattSvcDevs;
    if (mBattSvcDevs)
        mBattSvcDevs->prev = svc;
    mBattSvcDevs = svc;

    pthread_mutex_unlock(&mBattSvcLock);
    mCbkState(svcConn, BTLE_BATT_SVC_CONN_STATE_GATT_DISCOVERY);

    logi("Battery Service attempting an attachment to GATT conn "GATTHANDLEFMT"\n", GATTHANDLECNV(gattConn));
    if (GATT_CLI_STATUS_OK == gattClientUtilFindAndTraversePrimaryService(NULL, gattConn, &battSvcUuid, 0, gattSvcBatteryAttachServiceTraversalCbk))
        return svcConn;

    logw("Failed to start the GATT service search\n");
    if (!gattSvcBattDetachFromGatt(svcConn))
        loge("Failed to detach on error on attach!\n");

    return 0;
}

/*
 * FUNCTION: gattSvcBattRequestRead
 * USE:      Request a battery reading
 * PARAMS:   handle - BattSvc connection handle
 * RETURN:   fals eon immediate error, else wait for GattSvcBattUpdateCbk() cbk
 * NOTES:
 */
bool gattSvcBattRequestRead(gatt_svc_batt_conn_t handle)
{
    struct BattSvcConn *svc;

    pthread_mutex_lock(&mBattSvcLock);
    svc = gattSvcBattFindConnById(handle);
    if (!svc)
        logw("Attempting to get BattSvc level from a nonexistent BattSvc instance\n");
    else if (svc->state != BTLE_BATT_SVC_CONN_STATE_UP)
        logw("Attempting to get BattSvc level from a BattSvc instance in state %u\n", svc->state);
    else {
        gatt_client_conn_t conn = svc->gattConn;
        uint16_t handle = svc->charValHandle;

        pthread_mutex_unlock(&mBattSvcLock);
        return GATT_CLI_STATUS_OK == gattClientRead(NULL, conn, handle, 0, 0, 0, gattSvcBattReadReportReadCbk);
    }
    pthread_mutex_unlock(&mBattSvcLock);

    return false;
}

/*
 * FUNCTION: gattSvcBattIsNotifSupported
 * USE:      See if a given BattSvc instance supports async notifs
 * PARAMS:   handle - BattSvc connection handle
 * RETURN:   true if yes, false on error or NO
 * NOTES:
 */
bool gattSvcBattIsNotifSupported(gatt_svc_batt_conn_t handle)
{
    struct BattSvcConn *svc;
    bool ret = false;

    pthread_mutex_lock(&mBattSvcLock);
    svc = gattSvcBattFindConnById(handle);
    if (!svc)
        logw("Attempting to get BattSvc info from a nonexistent BattSvc instance\n");
    else if (svc->state != BTLE_BATT_SVC_CONN_STATE_UP)
        logw("Attempting to get BattSvc info from a BattSvc instance in state %u\n", svc->state);
    else
        ret = !!svc->charCccdHandle;
    pthread_mutex_unlock(&mBattSvcLock);

    return ret;
}

/*
 * FUNCTION: gattSvcBattInit
 * USE:      Init BattSvc subsystem
 * PARAMS:   stateCbk - the state callback
 *           rxCbk - the data RX callback
 * RETURN:   the structure or NULL if not found
 * NOTES:    call with mHidLock held
 */
void gattSvcBattInit(GattSvcBattConnStateCbk stateCbk, GattSvcBattUpdateCbk rxCbk)
{
    mCbkState = stateCbk;
    mCbkRx = rxCbk;
}


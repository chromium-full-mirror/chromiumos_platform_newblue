#include <string.h>
#include "gatt-builtin.h"
#include "l2cap.h"
#include "gatt.h"
#include "util.h"
#include "uuid.h"
#include "att.h"
#include "hci.h"
#include "log.h"
#include "mt.h"


/* services */
static gatt_service_t mSvcGatt = 0;
static gatt_service_t mSvcGap = 0;

/* handles */
static uint16_t mSvcGapDevName;
static uint16_t mSvcGapAppearance;
static uint16_t mSvcGapPrefConnParams;

/* data */
static pthread_mutex_t mGattBuiltinLock = PTHREAD_MUTEX_INITIALIZER;
static char mGapName[HCI_DEV_NAME_LEN + 1] = {0,};
static uint16_t mGapAppearance;
static struct {
    uint16_t intMin;
    uint16_t intMax;
    uint16_t lat;
    uint16_t timeout;
}  __packed mGapPrefConnParams;


#define GATT_UUID_GAP_DEV_NAME                0x2A00
#define GATT_UUID_GAP_APPEARANCE              0x2A01
#define GATT_UUID_GAP_PREFERRED_CONN_PARAMS   0x2A04



/*
 * FUNCTION: gattBuiltinReadF
 * USE:      GATT's callback for reading
 * PARAMS:   svc - service ref
 *           who - connection to whom this pertains to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - the handle that this pertains to
 *           byteOfst - the offset into the characteristic for the read (in bytes)
 *           reason - reason for the write (ATT_READ_FOR_*)
 *           wantedLen - the length to write
 * RETURN:   NONE
 * NOTES:    call gattSrvCbkReply() with results. returning false will send ATT_ERROR_UNLIKELY_ERROR to the client
 */
static bool gattBuiltinReadF(gatt_service_t svc, l2c_handle_t who, att_cid_t cid, att_trans_t transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t wantedLen)
{
    uint8_t err = ATT_ERROR_NONE;
    const uint8_t *ptr = NULL;
    uint16_t len = 0;

    if (svc != mSvcGap) {
        logw("Unexpected read in non-GAP service\n");
        return false;
    }

    if (handle == mSvcGapDevName) {
        ptr = (const uint8_t*)mGapName;
        len = strlen(mGapName);
    } else if (handle == mSvcGapAppearance) {
        ptr = (const uint8_t*)&mGapAppearance;
        len = sizeof(mGapAppearance);
    } else if (handle == mSvcGapPrefConnParams) {
        ptr = (const uint8_t*)&mGapPrefConnParams;
        len = sizeof(mGapPrefConnParams);
    } else {
        logw("Unexpected read from handle %u of svc "GATTHANDLEFMT" from conn "HANDLEFMT"\n", handle, GATTHANDLECNV(svc), HANDLECNV(who));
        return false;
    }

    if (byteOfst > len)
        err = ATT_ERROR_INVALID_OFFSET;
    else {
        ptr += byteOfst;
        len -= byteOfst;
    }
    if (len > wantedLen)
        len = wantedLen;

    gattSrvCbkReply(cid, transId, handle, err, ptr, len);
    return true;
}

/*
 * FUNCTION: gattBuiltinWriteF
 * USE:      GATT's callback for writing
 * PARAMS:   svc - service ref
 *           who - connection to whom this pertains to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - the handle that this pertains to
 *           byteOfst - the offset into the characteristic for the write (in bytes)
 *           reason - reason for the write (ATT_WRITE_FOR_*)
 *           len - the length to write
 *           data - the data to write
 * RETURN:   NONE
 * NOTES:    this callback is unused here
 */
static bool gattBuiltinWriteF(gatt_service_t svc, l2c_handle_t who, att_cid_t cid, att_trans_t transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    /* in reality we can never get here since GATT & ATT enforce for us the read-only-ness of other handles */
    logw("Unexpected write to handle %u of svc "GATTHANDLEFMT" from conn "HANDLEFMT"\n", handle, GATTHANDLECNV(svc), HANDLECNV(who));
    return false;
}

/*
 * FUNCTION: gattBuiltinIndF
 * USE:      GATT's callback for indication/notification progress
 * PARAMS:   svc - service ref
 *           who - connection to whom this pertains to
 *           cid - ATT's connection ID
 *           handle - the handle that this pertains to
 *           evt - what happened (GATT_SRV_EVT_*)
 *           ref - value originally passed to gattServiceSendInd()
 * RETURN:   NONE
 * NOTES:    this callback is unused here
 */
static void gattBuiltinIndF(gatt_service_t svc, l2c_handle_t who, att_cid_t cid, uint16_t handle, uint8_t evt, uint64_t ref) /* GATT_SRV_EVT_* */
{
    logw("Unexpected ind notification for %u of svc "GATTHANDLEFMT" from conn "HANDLEFMT"\n", handle, GATTHANDLECNV(svc), HANDLECNV(who));
    return;
}

/*
 * FUNCTION: gattBuiltinInit
 * USE:      Init the gatt builtin profiles
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
bool gattBuiltinInit(void)
{
    struct uuid uuid;

    /* make "GATT" service */
    uuidFromUuid16(&uuid, GATT_UUID_SVC_GATT_SVR);
    mSvcGatt = gattServiceCreate(&uuid, true, 1, gattBuiltinReadF, gattBuiltinWriteF, gattBuiltinIndF);
    if (!mSvcGatt) {
        loge("Failed to register GATT.GATT\n");
        goto out_fail;
    }

    /* make "GAP" service */
    uuidFromUuid16(&uuid, GATT_UUID_SVC_GAP);
    mSvcGap = gattServiceCreate(&uuid, true, 7, gattBuiltinReadF, gattBuiltinWriteF, gattBuiltinIndF);
    if (!mSvcGap) {
        loge("Failed to register GATT.GAP\n");
        goto out_gatt_destroy;
    }

    /* add the "Device Name" characteristic */
    uuidFromUuid16(&uuid, GATT_UUID_GAP_DEV_NAME);
    mSvcGapDevName = gattServiceAddChar(mSvcGap, &uuid, GATT_PROP_READ, GATT_PERM_ALL_READ);
    if (!mSvcGapDevName) {
        loge("Failed to add GATT.GAP.DevName char\n");
        goto out_gap_destroy;
    }

    /* add the "Appearance" characteristic */
    uuidFromUuid16(&uuid, GATT_UUID_GAP_APPEARANCE);
    mSvcGapAppearance = gattServiceAddChar(mSvcGap, &uuid, GATT_PROP_READ, GATT_PERM_ALL_READ);
    if (!mSvcGapAppearance) {
        loge("Failed to add GATT.GAP.Appearance char\n");
        goto out_gap_destroy;
    }

    /* add the "Preferred connection params" characteristic */
    uuidFromUuid16(&uuid, GATT_UUID_GAP_PREFERRED_CONN_PARAMS);
    mSvcGapPrefConnParams = gattServiceAddChar(mSvcGap, &uuid, GATT_PROP_READ, GATT_PERM_ALL_READ);
    if (!mSvcGapPrefConnParams) {
        loge("Failed to add GATT.GAP.PrefConnParams char\n");
        goto out_gap_destroy;
    };

    if (!gattServiceStart(mSvcGatt, true, true)) {
        loge("Failed to start GATT.GATT\n");
        goto out_gap_destroy;
    }

    if (!gattServiceStart(mSvcGap, true, true)) {
        loge("Failed to start GATT.GAP\n");
        goto out_gatt_stop;
    }

    logd("GATT.GATT created as svc "GATTHANDLEFMT" at handle 0x%04X\n", GATTHANDLECNV(mSvcGatt), gattServiceGetHandleBaseBySvc(mSvcGatt));
    logd("GATT.GAP created as svc "GATTHANDLEFMT" at handle 0x%04X\n", GATTHANDLECNV(mSvcGap), gattServiceGetHandleBaseBySvc(mSvcGap));
    return true;

out_gatt_stop:
    gattServiceStop(mSvcGatt);

out_gap_destroy:
    gattServiceDestroy(mSvcGap);

out_gatt_destroy:
    gattServiceDestroy(mSvcGatt);

out_fail:
    return false;
}

/*
 * FUNCTION: gattBuiltinDeinit
 * USE:      Deinit the gatt builtin profiles
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void gattBuiltinDeinit(void)
{
    gattServiceStop(mSvcGap);
    gattServiceStop(mSvcGatt);
    gattServiceDestroy(mSvcGap);
    gattServiceDestroy(mSvcGatt);
    mSvcGatt = mSvcGap = 0;
}

/*
 * FUNCTION: gattBuiltinSetDevName
 * USE:      Set device name visible on GAP
 * PARAMS:   name - the bt name in the usual format (HCI_DEV_NAME_LEN chars, NULL terminated unless exactly HCI_DEV_NAME_LEN chars long)
 * RETURN:   NONE
 * NOTES:    can be called before gattBuiltinInit or after gattBuiltinDeinit. Must handle that!
 */
void gattBuiltinSetDevName(const char *name)
{
    uint8_t len = strnlen(name, HCI_DEV_NAME_LEN);

    pthread_mutex_lock(&mGattBuiltinLock);
    memcpy(mGapName, name, len);
    mGapName[len] = 0;
    pthread_mutex_unlock(&mGattBuiltinLock);
}

/*
 * FUNCTION: gattBuiltinSetPreferredConnParamsWhenSlave
 * USE:      Set preferred conenction params visible on GAP
 * PARAMS:   minInt - preferred minimum interval 
 *           maxInt - preferred max interval
 *           lat - preferred latency
 *           to - preferred timeout
 * RETURN:   NONE
 * NOTES:    units for all params are standard LE connection units/multiples
 *           can be called before gattBuiltinInit or after gattBuiltinDeinit. Must handle that!
 */
void gattBuiltinSetPreferredConnParamsWhenSlave(uint16_t minInt, uint16_t maxInt, uint16_t lat, uint16_t to)
{
    pthread_mutex_lock(&mGattBuiltinLock);
    utilSetLE16(&mGapPrefConnParams.intMin, minInt);
    utilSetLE16(&mGapPrefConnParams.intMax, maxInt);
    utilSetLE16(&mGapPrefConnParams.lat, lat);
    utilSetLE16(&mGapPrefConnParams.timeout, to);
    pthread_mutex_unlock(&mGattBuiltinLock);
}

/*
 * FUNCTION: gattBuiltinSetAppearance
 * USE:      Set "device appearance" as visible on GAP
 * PARAMS:   appearance - GAP_APPEAR_*
 * RETURN:   NONE
 * NOTES:    can be called before gattBuiltinInit or after gattBuiltinDeinit. Must handle that!
 */
void gattBuiltinSetAppearance(uint16_t appearance)
{
    pthread_mutex_lock(&mGattBuiltinLock);
    utilSetLE16(&mGapAppearance, appearance);
    pthread_mutex_unlock(&mGattBuiltinLock);
}



#include <pthread.h>
#include <stdlib.h>
#include "aapiSocket.h"
#include "aapiGatt.h"
#include "workQueue.h"
#include "persist.h"
#include "rfcomm.h"
#include "config.h"
#include "timer.h"
#include "l2cap.h"
#include "aapi.h"
#include "hci.h"
#include "log.h"
#include "sdp.h"
#include "bt.h"
#include "mt.h"


/* a device will never hove more than this many properties */
#define BT_MAX_PROPS    32


struct aapiWorkItem {
    uint8_t type;
    union {
        struct {
            bool on;
        } state;
        struct properties {
            bt_status_t status;
            bt_bdaddr_t bd_addr;
            int num_properties;
            bt_property_t properties[];
        } properties;
        struct {
            bt_bdaddr_t remote_bd_addr;
            bt_bdname_t bd_name;
            uint32_t cod;
        } pinReq;
        struct {
            bt_bdaddr_t remote_bd_addr;
            bt_bdname_t bd_name;
            uint32_t cod;
            bt_ssp_variant_t pairing_variant;
            uint32_t pass_key;
        } sspReq;
        struct {
            bt_status_t status;
            bt_bdaddr_t remote_bd_addr;
            uint32_t state;
        } stateCbk;
        struct {
            uint16_t opcode;
            uint8_t len;
            uint8_t buf[];
        } hciEvt;
        struct {
            bt_status_t status;
            uint16_t num_packets;
        } leTestEvt;
    };
};

#define WT_ADAPTER_STATE_CHG    0
#define WT_ADAPTER_PROPS        1
#define WT_REMOTE_PROPS         2
#define WT_DEV_DISCOVERED       3
#define WT_DISC_STATE_CHG       4
#define WT_PIN_REQ              5
#define WT_SSP_REQ              6
#define WT_BOND_STATE_CHG       7
#define WT_ACL_STATE_CHG        8
#define WT_DUT_EVT              9
#define WT_LE_TEST_EVT          10








/* our state */
static pthread_t mWorker;
static struct workQueue *mWorkQ;
static pthread_mutex_t mStateLock = PTHREAD_MUTEX_INITIALIZER;
static bool mStackUp = false;

/* properties java can get/set */
//TODO: mutex around these!
static bt_bdaddr_t mLocalMac = {{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
static bt_bdname_t mLocalName;
static uint32_t mDevCls = 0xCC0704; //wristwatch (0x20C is smartphone)
static bt_scan_mode_t mScanMode = BT_SCAN_MODE_NONE;
static bt_device_type_t mDevType = BT_DEVICE_DEVTYPE_DUAL;
static uint32_t mDiscoveryTimeout = 60 ;/* in seconds */


/*
 * FUNCTION: aapiIsStackUp
 * USE:      Find out if stack is up
 * PARAMS:   none
 * RETURN:   the answer
 * NOTES:
 */
bool aapiIsStackUp(void)
{
    bool up;

    pthread_mutex_lock(&mStateLock);
    up = mStackUp;
    pthread_mutex_unlock(&mStateLock);
    logd("aapiIsStackUp()->%s\n", up ? "true" : "false");
    return up;
}

/*
 * FUNCTION: aapiHciLogEnable
 * USE:      Enable/disable HCI logging
 * PARAMS:   enable - on or off?
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiHciLogEnable(uint8_t enable)
{
    logd("AAPI: hci log enable(%u)\n", enable);
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiLeTestMode
 * USE:      Send an LE test command
 * PARAMS:   opcode - the opcode
 *           buf - the command params
 *           len - length of said params
 * RETURN:   bt_status_t
 * NOTES:    LE_Receiver_Test, LE_Transmitter_Test, LE_Test_End commands only
 *           Does not require DUT mode enabled (?)
 */
static int aapiLeTestMode(uint16_t opcode, uint8_t *buf, uint8_t len)
{
    logd("AAPI: le test mode (0x%04X, %ub)\n", opcode, len);
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiDutSend
 * USE:      Send any command to the controller
 * PARAMS:   opcode - the opcode
 *           buf - the command params
 *           len - length of said params
 * RETURN:   bt_status_t
 * NOTES:    intended only for vendor test commands, only valid after DUT mode enabled by .dut_mode_configure
 */
static int aapiDutSend(uint16_t opcode, uint8_t *buf, uint8_t len)
{
    logd("AAPI: DUT send (0x%04X, %ub)\n", opcode, len);
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiDutEnable
 * USE:      Enable/disable DUT mode (used for debugging/testing)
 * PARAMS:   enable - on or off?
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiDutEnable(uint8_t enable)
{
    logd("AAPI: dut enable(%u)\n", enable);
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiSspReply
 * USE:      SSP pairing
 * PARAMS:   bd_addr - the peer adress
 *           variant- pairing type being used
 *           accept - pairing was cancelled by user
 *           passkey - entered code
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiSspReply(const bt_bdaddr_t *bd_addr, bt_ssp_variant_t variant, uint8_t accept, uint32_t passkey)
{
    struct bt_addr addr;
    logd("AAPI: ssp reply\n");

    memcpy(addr.addr, bd_addr->address, sizeof(addr.addr));
    addr.type = BT_ADDR_TYPE_EDR; /* XXX: this is a bug in android's api, but we must live with it (for now) */

    switch (variant) {
    case BT_SSP_VARIANT_PASSKEY_CONFIRMATION:
        hciSecUserCbkSspUserConfirmationReq(&addr, accept);
        break;
    case BT_SSP_VARIANT_PASSKEY_ENTRY:
        hciSecUserCbkSspUserPasskeyReq(&addr, accept ? passkey : HCI_SSP_NUMERIC_INVALID);
        break;
    case BT_SSP_VARIANT_CONSENT:
    case BT_SSP_VARIANT_PASSKEY_NOTIFICATION:
        logw("unexpected pair type %u\n", variant);
        break;
    }
    return 0;
}

/*
 * FUNCTION: aapiPinReply
 * USE:      Legacy pairing
 * PARAMS:   bd_addr - the peer adress
 *           accept - pairing was cancelled by user
 *           pin_len - length of entered pin
 *           pin_code - entered code
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiPinReply(const bt_bdaddr_t *bd_addr, uint8_t accept, uint8_t pin_len, bt_pin_code_t *pin_code)
{
    struct bt_addr addr;
    logd("AAPI: pin reply(%ub)\n", pin_len);
 
    memcpy(addr.addr, bd_addr->address, sizeof(addr.addr));
    addr.type = BT_ADDR_TYPE_EDR; /* XXX: this is a bug in android's api, but we must live with it (for now) */

    hciSecUserCbkPinCodeReq(&addr, accept ? pin_code->pin : NULL, accept ? pin_len : 0);
    return 0;
}

/*
 * FUNCTION: aapiBondCreate
 * USE:      Start a pairing process
 * PARAMS:   bd_addr - the peer adress
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiBondCreate(const bt_bdaddr_t *bd_addr)
{
    logd("AAPI: bond create\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiBondRemove
 * USE:      Delete pairing info
 * PARAMS:   bd_addr - the peer adress
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiBondRemove(const bt_bdaddr_t *bd_addr)
{
    logd("AAPI: bond remove\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiBondCancel
 * USE:      Cancel a pairing process
 * PARAMS:   bd_addr - the peer adress
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiBondCancel(const bt_bdaddr_t *bd_addr)
{
    logd("AAPI: bond cancel\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiDiscoveryStart
 * USE:      Start a discovery
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiDiscoveryStart(void)
{
    logd("AAPI: discovery start\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiDiscoveryStop
 * USE:      Cancel an ongoing discovery
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiDiscoveryStop(void)
{
    logd("AAPI: discovery stop\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiGetRemoteServices
 * USE:      Find serives at the peer
 * PARAMS:   remote_addr - the device
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiGetRemoteServices(bt_bdaddr_t *remote_addr)
{
    logd("AAPI: get remote services\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiGetRemoteServiceRecord
 * USE:      Find and retrieve a service record from a peer
 * PARAMS:   remote_addr - the device
 *           uuid - the UUID to search for
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiGetRemoteServiceRecord(bt_bdaddr_t *remote_addr, bt_uuid_t *uuid)
{
    logd("AAPI: get remote service record\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiGetRemoteDevProperties
 * USE:      Get all of a remote device's properties
 * PARAMS:   remote_addr - the device
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiGetRemoteDevProperties(bt_bdaddr_t *remote_addr)
{
    logd("AAPI: get remote dev properties\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiGetRemoteDevProperty
 * USE:      Get a remote device's property
 * PARAMS:   remote_addr - the device
 *           type - the property to get
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiGetRemoteDevProperty(bt_bdaddr_t *remote_addr, bt_property_type_t type)
{
    logd("AAPI: get remote dev prop\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiSetRemoteDevProperty
 * USE:      Set a remote device's property
 * PARAMS:   remote_addr - the device
 *           property - the property to set
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiSetRemoteDevProperty(bt_bdaddr_t *remote_addr, const bt_property_t *property)
{
    logd("AAPI: set remote dev prop\n");
    //TODO
    return 0;
}

/*
 * FUNCTION: aapiGetSomeAdapterProperties
 * USE:      Get all or one of Bluetooth Adapter properties
 * PARAMS:   statToSend - what status value to send
 *           typeP - points to prop typ ewe want, or NULL for all
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiGetSomeAdapterProperties(bt_status_t statToSend, const bt_property_type_t *typeP)
{
    static bt_uuid_t uuids[] = {{{0}}}; /* TODO */
    static bt_bdaddr_t bondedDevs = {{0,}}; /* TODO */
    bt_property_t props[BT_MAX_PROPS];
    unsigned numProps = 0, numUuids = 0, numBondedDevs = 0;

    if (!typeP || *typeP == BT_PROPERTY_BDNAME) {
        props[numProps].type = BT_PROPERTY_BDNAME;
        props[numProps].len = strnlen((const char*)mLocalName.name, sizeof(mLocalName.name));
        props[numProps].val = &mLocalName;
        numProps++;
    }

    if (!typeP || *typeP == BT_PROPERTY_BDADDR) {
        props[numProps].type = BT_PROPERTY_BDADDR;
        props[numProps].len = sizeof(mLocalMac);
        props[numProps].val = &mLocalMac;
        numProps++;
    }

    if (!typeP || *typeP == BT_PROPERTY_UUIDS) {
        props[numProps].type = BT_PROPERTY_UUIDS;
        props[numProps].len = sizeof(bt_uuid_t[numUuids]);
        props[numProps].val = &uuids;
        numProps++;
    }

    if (!typeP || *typeP == BT_PROPERTY_CLASS_OF_DEVICE) {
        props[numProps].type = BT_PROPERTY_CLASS_OF_DEVICE;
        props[numProps].len = sizeof(mDevCls);
        props[numProps].val = &mDevCls;
        numProps++;
    }

    if (!typeP || *typeP == BT_PROPERTY_TYPE_OF_DEVICE) {
        props[numProps].type = BT_PROPERTY_TYPE_OF_DEVICE;
        props[numProps].len = sizeof(mDevType);
        props[numProps].val = &mDevType;
        numProps++;
    }

    /* TODO: BT_PROPERTY_SERVICE_RECORD here ? */

    if (!typeP || *typeP == BT_PROPERTY_ADAPTER_SCAN_MODE) {
        props[numProps].type = BT_PROPERTY_ADAPTER_SCAN_MODE;
        props[numProps].len = sizeof(mScanMode);
        props[numProps].val = &mScanMode;
        numProps++;
    }

    if (!typeP || *typeP == BT_PROPERTY_ADAPTER_BONDED_DEVICES) {
        props[numProps].type = BT_PROPERTY_ADAPTER_BONDED_DEVICES;
        props[numProps].len = sizeof(bt_bdaddr_t[numBondedDevs]);
        props[numProps].val = &bondedDevs;
        numProps++;
    }

    if (!typeP || *typeP == BT_PROPERTY_ADAPTER_DISCOVERY_TIMEOUT) {
        props[numProps].type = BT_PROPERTY_ADAPTER_DISCOVERY_TIMEOUT;
        props[numProps].len = sizeof(mDiscoveryTimeout);
        props[numProps].val = &mDiscoveryTimeout;
        numProps++;
    }


    aapiAdapterProperties(statToSend, numProps, props);
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGetAdapterProperty
 * USE:      Get a Bluetooth Adapter property
 * PARAMS:   type - the property to get
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiGetAdapterProperty(bt_property_type_t type)
{
    logd("AAPI: get adapter property(%u)\n", type);
    return aapiGetSomeAdapterProperties(BT_STATUS_SUCCESS, &type);
}

/*
 * FUNCTION: aapiGetAdapterProperties
 * USE:      Get all Bluetooth Adapter properties at init
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiGetAdapterProperties(void)
{
    logd("AAPI: get adapter properties\n");
    return aapiGetSomeAdapterProperties(BT_STATUS_SUCCESS, NULL);
}

/*
 * FUNCTION: aapiSetPropOpDoneCbk
 * USE:      Done calback for "set property" calls to hci
 * PARAMS:   cbkData - in our case the property we tried to set
 *           status - the status from the chip (0 = good, else bad)
 * RETURN:   NONE
 * NOTES:    calls back to java with success and new property value
 */
static void aapiSetPropOpDoneCbk(void *cbkData, uint8_t status)
{
    bt_property_type_t prop = (bt_property_type_t)cbkData;

    logd("Prop %u set with status %u\n", prop, status);
    aapiGetSomeAdapterProperties(status ? BT_STATUS_FAIL : BT_STATUS_SUCCESS, &prop);
}

/*
 * FUNCTION: aapiSetAdapterProperty
 * USE:      Set a Bluetooth Adapter property
 * PARAMS:   property - the property to set
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiSetAdapterProperty(const bt_property_t *property)
{
    uint32_t len;
    logd("AAPI: set adapter prop (%u)\n", property->type);

    switch (property->type) {
    case BT_PROPERTY_BDNAME:
        memset(&mLocalName, 0, sizeof(mLocalName));
        len = property->len;
        if (len > sizeof(mLocalName)) {
            len = sizeof(mLocalName);
            logw("Name truncated\n");
        }
        memcpy(&mLocalName, property->val, len);
        if (!persistSetDeviceName(mLocalName.name, len))
            logw("Failed to save BT name\n");
        if (hciSetLocalName((const char*)mLocalName.name, aapiSetPropOpDoneCbk, (void*)BT_PROPERTY_BDNAME))
            break;
        loge("Set name up failed\n");
        return BT_STATUS_FAIL;
    case BT_PROPERTY_ADAPTER_SCAN_MODE:
        if (property->len != sizeof(mScanMode)) {
            loge("refusing set scan mode with strange len (%u != %u)\n", property->len, sizeof(mScanMode));
            return BT_STATUS_FAIL;
        }
        memcpy(&mScanMode, property->val, sizeof(mScanMode));
        bool discoverable = mScanMode == BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE;
        bool connectible = mScanMode != BT_SCAN_MODE_NONE;
        if (hciSetDiscoverableConnectable(&discoverable, &connectible, aapiSetPropOpDoneCbk, (void*)BT_PROPERTY_ADAPTER_SCAN_MODE))
            break;
        loge("Set discoverable/connectible up failed\n");
        return BT_STATUS_FAIL;

    case BT_PROPERTY_ADAPTER_DISCOVERY_TIMEOUT:
        if (property->len != sizeof(mDiscoveryTimeout)) {
            loge("refusing set discovery timeout with strange len (%u != %u)\n", property->len, sizeof(mDiscoveryTimeout));
            return BT_STATUS_FAIL;
        }
        memcpy(&mDiscoveryTimeout, property->val, sizeof(mDiscoveryTimeout));
        if (!persistSetDiscoveryLength(mDiscoveryTimeout))
            logw("Failed to save discovery timeout\n");
        return aapiGetSomeAdapterProperties(BT_STATUS_SUCCESS, &property->type);

    default:
        loge("Refusing to set unknown local property %u\n", property->type);
        return BT_STATUS_FAIL;
    }

    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiEnableOpDoneCbk
 * USE:      Done calback for a step in the init process
 * PARAMS:   cbkData - in our case the step number
 *           status - the status from the chip (0 = good, else bad)
 * RETURN:   NONE
 * NOTES:    calls next step or calls java to say we're done (success or fail is conveyed too)
 */
static void aapiEnableOpDoneCbk(void *cbkData, uint8_t status)
{
    unsigned step = (unsigned)cbkData;

    if (status) {
        loge("Enable failed on step %u with status %u\n", step, status);
        aapiAdapterStateChanged(false);
    }

    logd("aapi init step %u done\n", step);
    switch (step) {
    case 0:
        if (hciSetDeviceClass(mDevCls, aapiEnableOpDoneCbk, (void*)(step + 1)))
            return;
        break;
    case 1:;
        bool discoverable = mScanMode == BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE;
        bool connectible = mScanMode != BT_SCAN_MODE_NONE;
        if (hciSetDiscoverableConnectable(&discoverable, &connectible, aapiEnableOpDoneCbk, (void*)(step + 1)))
            return;
        break;
    case 2:
        aapiAdapterStateChanged(true);
        return;
    default:
        loge("Unknown init step %u\n", step);
        aapiAdapterStateChanged(false);
        return;
    }

    loge("Error after step %u\n", step);
    aapiAdapterStateChanged(false);
}

/*
 * FUNCTION: aapiEnable
 * USE:      Enable BT
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiEnable(void)
{
    int ret;

    logd("AAPI: enable\n");
    pthread_mutex_lock(&mStateLock);
    if (mStackUp) {
        loge("stack requested to come up while already up\n");
        goto fail_up;
    }

    persistLoad();
    mLocalName.name[persistGetDeviceName(mLocalName.name)] = 0;
    mDiscoveryTimeout = persistGetDiscoveryLength();

    if (!timersInit()) {
        loge("timers failed\n");
        goto fail_timers;
    }

    if (!hciUp(mLocalMac.address)) {
        loge("HCI up failed\n");
        goto fail_hci;
    }

    if (hciInfoAclBufSizeEdr(NULL, NULL) &&  hciInfoAclBufSizeLe(NULL, NULL))
        mDevType = BT_DEVICE_DEVTYPE_DUAL;
    else if (hciInfoAclBufSizeLe(NULL, NULL))
        mDevType = BT_DEVICE_DEVTYPE_BLE;
    else if (hciInfoAclBufSizeEdr(NULL, NULL))
        mDevType = BT_DEVICE_DEVTYPE_BREDR;
    else {
        mDevType = 0;
        loge("device type indeterminate\n");
    }

    ret = l2cInit();
    if (ret) {
        loge("L2C up failed\n");
        goto fail_l2c;
    }

    if (!sdpInit()) {
        loge("SDP up failed\n");
        goto fail_sdp;
    }

    if (!rfcInit()) {
        loge("RFC up failed\n");
        goto fail_rfc;
    }

    if (!hciSetLocalName((const char*)mLocalName.name, aapiEnableOpDoneCbk, (void*)0)) {
        loge("Set name up failed\n");
        goto fail_name;
    }

    /* more enable here if needed */
    aapiAdapterStateChanged(true);
    aapiGetAdapterProperties(); /* contrary to api, this must be here */

    mStackUp = true;
    aapiSocketNotifStackState(true);
    aapiGattNotifStackState(true);
    pthread_mutex_unlock(&mStateLock);

    return BT_STATUS_SUCCESS;

fail_name:
    rfcDeinit();

fail_rfc:
    sdpDeinit();

fail_sdp:
    l2cDeinit();

fail_l2c:
    hciDown();

fail_hci:
    timersDeinit();

fail_timers:
fail_up:
    mStackUp = false;
    aapiSocketNotifStackState(false);
    aapiGattNotifStackState(false);
    persistStore();
    pthread_mutex_unlock(&mStateLock);
    return BT_STATUS_FAIL;
}

/*
 * FUNCTION: aapiDisable
 * USE:      Disable BT
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiDisable(void)
{
    logd("AAPI: disable\n");
    pthread_mutex_lock(&mStateLock);
    if (!mStackUp)
        loge("Cannot turn stack off while off\n");
    else {
        mStackUp = false;
        rfcDeinit();
        sdpDeinit();
        l2cDeinit();
        hciDown();
        timersDeinit();
    }
    aapiSocketNotifStackState(false);
    aapiGattNotifStackState(false);
    persistStore();
    pthread_mutex_unlock(&mStateLock);
    return 0;
}

/*
 * FUNCTION: aapiGetProfileIface
 * USE:      Get a pointer to a particular profile's interface
 * PARAMS:   profile_id - the profile
 * RETURN:   bt_status_t
 * NOTES:
 */
static const void* aapiGetProfileIface(const char *profile_id)
{
//    if (!strcmp(profile_id, BT_PROFILE_HANDSFREE_ID))
//        return &pHandsfree;
    if (!strcmp(profile_id, BT_PROFILE_SOCKETS_ID))
        return aapiGetProfileIfaceSockets();
//    if (!strcmp(profile_id, BT_PROFILE_PAN_ID))
//        return &pPan;
//    if (!strcmp(profile_id, BT_PROFILE_ADVANCED_AUDIO_ID))
//        return &pAdvAudio;
//    if (!strcmp(profile_id, BT_PROFILE_HIDHOST_ID))
//        return &pHidHost;
//    if (!strcmp(profile_id, BT_PROFILE_HEALTH_ID))
//        return &pHealth;
    if (!strcmp(profile_id, BT_PROFILE_GATT_ID))
        return aapiGetProfileIfaceGatt();
//    if (!strcmp(profile_id, BT_PROFILE_AV_RC_ID))
//        return &pAvrcp;

    logw("Asked for unknown profile '%s'\n", profile_id);
    return NULL;
}

/*
 * FUNCTION: aapiWorker
 * USE:      BT worker thread for callbacks to java
 * PARAMS:   callbacks - the callbacks to the higher layer
 * RETURN:   unused
 * NOTES:
 */
static void* btWorker(void *callbacks)
{
    bt_callbacks_t *cbks = (bt_callbacks_t*)callbacks;
    struct aapiWorkItem *wi;
    int status;

    pthread_setname_np(pthread_self(), "aapi_worker");
    cbks->thread_evt_cb(ASSOCIATE_JVM);

    while(1) {

        status = workQueueGet(mWorkQ, (void**)&wi);

        if (status)
            break;

        logd("callback type %u\n", wi->type);
        switch (wi->type) {
        case WT_ADAPTER_STATE_CHG:
            cbks->adapter_state_changed_cb(wi->state.on ? BT_STATE_ON : BT_STATE_OFF);
            break;
        case WT_ADAPTER_PROPS:
            cbks->adapter_properties_cb(wi->properties.status, wi->properties.num_properties, wi->properties.properties);
            break;
        case WT_REMOTE_PROPS:
            cbks->remote_device_properties_cb(wi->properties.status, &wi->properties.bd_addr, wi->properties.num_properties, wi->properties.properties);
            break;
        case WT_DEV_DISCOVERED:
            cbks->device_found_cb(wi->properties.num_properties, wi->properties.properties);
            break;
        case WT_DISC_STATE_CHG:
            cbks->discovery_state_changed_cb(wi->state.on ? BT_DISCOVERY_STARTED : BT_DISCOVERY_STOPPED);
            break;
        case WT_PIN_REQ:
            cbks->pin_request_cb(&wi->pinReq.remote_bd_addr, &wi->pinReq.bd_name, wi->pinReq.cod);
            break;
        case WT_SSP_REQ:
            cbks->ssp_request_cb(&wi->sspReq.remote_bd_addr,&wi->sspReq.bd_name, wi->sspReq.cod, wi->sspReq.pairing_variant, wi->sspReq.pass_key);
            break;
        case WT_BOND_STATE_CHG:
            cbks->bond_state_changed_cb(wi->stateCbk.status, &wi->stateCbk.remote_bd_addr, wi->stateCbk.state);
            break;
        case WT_ACL_STATE_CHG:
            cbks->acl_state_changed_cb(wi->stateCbk.status, &wi->stateCbk.remote_bd_addr, wi->stateCbk.state);
            break;
        case WT_DUT_EVT:
            cbks->dut_mode_recv_cb(wi->hciEvt.opcode, wi->hciEvt.buf, wi->hciEvt.len);
            break;
        case WT_LE_TEST_EVT:
            cbks->le_test_mode_cb(wi->leTestEvt.status, wi->leTestEvt.num_packets);
            break;
        default:
            logw("unknown work item type %u\n", wi->type);
            break;
        }
        free(wi);
    }

    cbks->thread_evt_cb(DISASSOCIATE_JVM);
    logd("aapi thread exiting\n");
    return NULL;
}

/*
 * FUNCTION: aapiWorkQueueFreeItem
 * USE:      Delete work items from the workQ on workQ deletion
 * PARAMS:   wi - the work item to free
 * RETURN:   NONE
 * NOTES:
 */
static void btWorkQueueFreeItem(void *wi)
{
    free(wi);
}

/*
 * FUNCTION: aapiInit
 * USE:      Init the stack (but not the chip), get callbacks
 * PARAMS:   callbacks - the callbacks to our user
 * RETURN:   bt_status_t
 * NOTES:    only stack-level init here (aka none)
 */
static int aapiInit(bt_callbacks_t* callbacks)
{
    static bt_callbacks_t cbks;

    memcpy(&cbks, callbacks, sizeof(cbks));

    mWorkQ = workQueueAlloc(AAPI_NUM_OUTSTANDING_WORK_ITEMS);
    if (!mWorkQ)
        return BT_STATUS_NOMEM;

    if (pthread_create(&mWorker, NULL, btWorker, &cbks)) {
        workQueueFree(mWorkQ, btWorkQueueFreeItem);
        return BT_STATUS_FAIL;
    }

    return 0;
}

/*
 * FUNCTION: aapiCleanup
 * USE:      Cleanup the stack and prepare for unloading
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void aapiCleanup(void)
{
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorker, NULL);
    workQueueFree(mWorkQ, btWorkQueueFreeItem);
}

/*
 * FUNCTION: aapiGetBtIface
 * USE:      Return the bt_interface_t struct full of ways to call us
 * PARAMS:   NONE
 * RETURN:   the bt_interface_t struct
 * NOTES:    no init here
 */
static const bt_interface_t* aapiGetBtIface(void)
{
    static const bt_interface_t iface = {
        .size = sizeof(bt_interface_t),
        .init = aapiInit,
        .cleanup = aapiCleanup,
        .enable = aapiEnable,
        .disable = aapiDisable,
        .get_adapter_properties = aapiGetAdapterProperties,
        .get_adapter_property = aapiGetAdapterProperty,
        .set_adapter_property = aapiSetAdapterProperty,
        .get_remote_device_properties = aapiGetRemoteDevProperties,
        .get_remote_device_property = aapiGetRemoteDevProperty,
        .set_remote_device_property = aapiSetRemoteDevProperty,
        .get_remote_service_record = aapiGetRemoteServiceRecord,
        .get_remote_services = aapiGetRemoteServices,
        .start_discovery = aapiDiscoveryStart,
        .cancel_discovery = aapiDiscoveryStop,
        .create_bond = aapiBondCreate,
        .remove_bond = aapiBondRemove,
        .cancel_bond = aapiBondCancel,
        .pin_reply = aapiPinReply,
        .ssp_reply = aapiSspReply,
        .get_profile_interface = aapiGetProfileIface,
        .dut_mode_configure = aapiDutEnable,
        .dut_mode_send = aapiDutSend,
        .le_test_mode = aapiLeTestMode,
        .config_hci_snoop_log = aapiHciLogEnable,
    };

    return &iface;
}

/*
 * FUNCTION: aapiClose
 * USE:      Module close function
 * PARAMS:   device - our bluetooth_device_t
 * RETURN:   0 on success
 * NOTES:    only module-level cleanup here (aka: none)
 */
static int aapiClose(struct hw_device_t* device)
{
    bluetooth_device_t *btd = (bluetooth_device_t*)device;

    //TODO: cleanup, if any
    return 0;
}

/*
 * FUNCTION: aapiOpen
 * USE:      Module open function
 * PARAMS:   module - the module object
 *           name - the module name
 *           abstractionP - we store our bluetooth_device_t there
 * RETURN:   0 on success
 * NOTES:    only module-level init here (aka none)
 */
static int aapiOpen(const struct hw_module_t* module, char const* name, struct hw_device_t** abstractionP)
{
    bluetooth_device_t *btd = (bluetooth_device_t*)calloc(1, sizeof(bluetooth_device_t));

    btd->common.tag = HARDWARE_DEVICE_TAG;
    btd->common.version = 0;
    btd->common.module = (struct hw_module_t*)module; /* casting const away is bad, but sadly such is our API */
    btd->common.close = aapiClose;
    btd->get_bluetooth_interface = aapiGetBtIface;

    *abstractionP = (struct hw_device_t*)btd;

    return 0;
}

/* these are needed for android module loader to open us */
static struct hw_module_methods_t mAapiMethods = {
    .open = aapiOpen,
};
extern hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = 0,
    .id = BT_HARDWARE_MODULE_ID,
    .name = "NewBlue stack",
    .author = "dmitrygr@",
    .methods = &mAapiMethods,
};


/*
 * FUNCTION: aapiWorkItemAlloc
 * USE:      Allocate a work item
 * PARAMS:   type - work item type
 *           extraSz - how many extra bytes to allocate
 * RETURN:   the pointer on success, else NULL
 * NOTES:    will print an error msg on error
 */
static struct aapiWorkItem* aapiWorkItemAlloc(uint8_t type, uint32_t extraSz)
{
    struct aapiWorkItem *wi = calloc(1, sizeof(struct aapiWorkItem) + extraSz);
    if (wi)
        wi->type = type;
    else
        loge("Failed to allocate work item type %u (+%ub)\n", type, extraSz);

    return wi;
}

/*
 * FUNCTION: aapiWorkItemEnqueue
 * USE:      Enqueue a work item
 * PARAMS:   wi - the work item
 * RETURN:   NONE
 * NOTES:    either enqueues or frees it. an error msg will be printed if needed
 */
static void aapiWorkItemEnqueue(struct aapiWorkItem *wi)
{
    if (workQueuePut(mWorkQ, wi))
        return;
    loge("Failed to enqueue work item type %u\n", wi->type);
    btWorkQueueFreeItem(wi);
}

/*
 * FUNCTION: aapiAdapterStateChanged
 * USE:      Notify android about BT adapter going up or coming down
 * PARAMS:   on - up or down?
 * RETURN:   NONE
 * NOTES:
 */
void aapiAdapterStateChanged(bool on)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_ADAPTER_STATE_CHG, 0);

    if (!wi)
        return;

    wi->state.on = on;
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiWorkItemAllocWithProperties
 * USE:      allocate a work item with a deep copy of all properties
 * PARAMS:   type - the work item type
 *           num_properties - how many properties we're sending
 *           properties - the properties to send (a copy is made)
 * RETURN:   the work item or NULL on error
 * NOTES:
 */
static struct aapiWorkItem* aapiWorkItemAllocWithProperties(uint8_t type, int num_properties, const bt_property_t *properties)
{
    struct aapiWorkItem* wi;
    uint32_t len = 0;
    bt_property_t *dstProps;
    uint8_t *dstDatas;
    int i;

    for (i = 0; i < num_properties; i++)
        len += sizeof(bt_property_t) + properties[i].len;

    wi = aapiWorkItemAlloc(WT_ADAPTER_PROPS, len);
    if (!wi)
        return NULL;

    dstProps = wi->properties.properties;
    dstDatas = (uint8_t*)(dstProps + num_properties);

    for (i = 0; i < num_properties; i++) {
        dstProps[i].type = properties[i].type;
        dstProps[i].len = properties[i].len;
        dstProps[i].val = dstDatas;
        memcpy(dstDatas, properties[i].val, properties[i].len);
        dstDatas += properties[i].len;
    }

    wi->properties.num_properties = num_properties;
    return wi;
}

/*
 * FUNCTION: aapiAdapterProperties
 * USE:      Notify android about BT adapter properties
 * PARAMS:   status - the status to send
 *           num_properties - how many properties we're sending
 *           properties - the properties to send (a copy is made)
 * RETURN:   NONE
 * NOTES:
 */
void aapiAdapterProperties(bt_status_t status, int num_properties, const bt_property_t *properties)
{
    struct aapiWorkItem* wi = aapiWorkItemAllocWithProperties(WT_ADAPTER_PROPS, num_properties, properties);

    if (!wi)
        return;

    wi->properties.status = status;
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiRemoteDevProperties
 * USE:      Notify android about remote device properties
 * PARAMS:   status - the status to send
 *           bd_addr - address of said device
 *           num_properties - how many properties we're sending
 *           properties - the properties to send (a copy is made)
 * RETURN:   NONE
 * NOTES:
 */
void aapiRemoteDevProperties(bt_status_t status, const bt_bdaddr_t *bd_addr, int num_properties, const bt_property_t *properties)
{
    struct aapiWorkItem* wi = aapiWorkItemAllocWithProperties(WT_REMOTE_PROPS, num_properties, properties);

    if (!wi)
        return;

    wi->properties.status = status;
    memcpy(&wi->properties.bd_addr, bd_addr, sizeof(wi->properties.bd_addr));
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiDevDiscoveredCbk
 * USE:      Notify android about a discovered device
 * PARAMS:   num_properties - how many properties we're sending
 *           properties - the properties to send (a copy is made)
 * RETURN:   NONE
 * NOTES:
 */
void aapiDevDiscoveredCbk(int num_properties, const bt_property_t *properties)
{
    struct aapiWorkItem* wi = aapiWorkItemAllocWithProperties(WT_DEV_DISCOVERED, num_properties, properties);

    if (!wi)
        return;

    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiDiscoveryStateChanged
 * USE:      Notify android about BT adapter starting or stopping discovery
 * PARAMS:   on - start or stop?
 * RETURN:   NONE
 * NOTES:
 */
void aapiDiscoveryStateChanged(bool on)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_DISC_STATE_CHG, 0);

    if (!wi)
        return;

    wi->state.on = on;
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiPinReqCbk
 * USE:      Notify android about a need for PIN entry UI
 * PARAMS:   peer - remote device address (a copy is made)
 * RETURN:   NONE
 * NOTES:
 */
void aapiPinReqCbk(const struct bt_addr *peer)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_PIN_REQ, 0);

    if (!wi)
        return;

    (void)persistGetKnownDev(peer, wi->pinReq.bd_name.name, NULL, &wi->pinReq.cod);

    memcpy(&wi->pinReq.remote_bd_addr.address, peer->addr, sizeof(wi->pinReq.remote_bd_addr.address));

    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiSspReqCbk
 * USE:      Notify android about a need for SSP pairing UI
 * PARAMS:   peer - remote device address (a copy is made)
 *           pairing_variant - pairing typ requested
 *           pass_key - the passkey to display, if relevant
 * RETURN:   NONE
 * NOTES:
 */
void aapiSspReqCbk(const struct bt_addr *peer, bt_ssp_variant_t pairing_variant, uint32_t pass_key)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_SSP_REQ, 0);

    if (!wi)
        return;

    (void)persistGetKnownDev(peer, wi->sspReq.bd_name.name, NULL, &wi->sspReq.cod);

    wi->sspReq.pairing_variant = pairing_variant;
    wi->sspReq.pass_key = pass_key;
    memcpy(&wi->sspReq.remote_bd_addr.address, peer->addr, sizeof(wi->sspReq.remote_bd_addr.address));

    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiBondStateChangedCbk
 * USE:      Notify android about bonding changes
 * PARAMS:   peer - remote device address (a copy is made)
 *           state - AAPI_BOND_STATE_*
 * RETURN:   NONE
 * NOTES:
 */
void aapiBondStateChangedCbk(const struct bt_addr *peer, uint8_t state)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_BOND_STATE_CHG, 0);

    if (!wi)
        return;

    switch (state) {
    case AAPI_BOND_STATE_FAILED:
        wi->stateCbk.status = AAPI_BOND_STATE_FAILED;
        wi->stateCbk.state = BT_BOND_STATE_NONE;
        break;
    case AAPI_BOND_STATE_INPROGRESS:
        wi->stateCbk.status = AAPI_BOND_STATE_SUCCESS;
        wi->stateCbk.state = BT_BOND_STATE_BONDING;
        break;
    case AAPI_BOND_STATE_SUCCESS:
        wi->stateCbk.status = AAPI_BOND_STATE_SUCCESS;
        wi->stateCbk.state = BT_BOND_STATE_BONDED;
        break;
    }

    memcpy(&wi->stateCbk.remote_bd_addr.address, peer->addr, sizeof(wi->stateCbk.remote_bd_addr.address));
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiAclStateChanged
 * USE:      Notify android about an ACL link being established
 * PARAMS:   status - the atatus
 *           peer - remote device address (a copy is made)
 *           up - was link brought up or did it come down
 * RETURN:   NONE
 * NOTES:
 */
void aapiAclStateChanged(bt_status_t status, const struct bt_addr *peer, bool up)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_ACL_STATE_CHG, 0);

    if (!wi)
        return;

    wi->stateCbk.status = status;
    wi->stateCbk.state = up ? BT_ACL_STATE_CONNECTED : BT_ACL_STATE_DISCONNECTED;
    memcpy(&wi->stateCbk.remote_bd_addr.address, peer->addr, sizeof(wi->stateCbk.remote_bd_addr.address));
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiDutModeEventCbk
 * USE:      Notify android about an event in response to a DUT-mode command
 * PARAMS:   opcode - the command opcode
 *           buf - the reply buffer (a copy is made)
 *           len - length of said buffer
 * RETURN:   NONE
 * NOTES:
 */
void aapiDutModeEventCbk(uint16_t opcode, const uint8_t *buf, uint8_t len)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_DUT_EVT, len);

    if (!wi)
        return;

    wi->hciEvt.opcode = opcode;
    wi->hciEvt.len = len;
    memcpy(&wi->hciEvt.buf, buf, len);
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiLeTestModeEventCbk
 * USE:      Notify android about an event in response to a LE test mode commands
 * PARAMS:   status - result of command
 *           num_packets - num_packets field from command complete event, if command was "end test"
 * RETURN:   NONE
 * NOTES:
 */
void aapiLeTestModeEventCbk(bt_status_t status, uint16_t num_packets)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_LE_TEST_EVT, 0);

    if (!wi)
        return;

    wi->leTestEvt.status = status;
    wi->leTestEvt.num_packets = num_packets;
    aapiWorkItemEnqueue(wi);
}


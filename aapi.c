#include <hardware/bluetooth.h>
#include <pthread.h>
#include <stdlib.h>
#include "aapiSocket.h"
#include "workQueue.h"
#include "aapi.h"
#include "config.h"
#include "log.h"





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


/*
 * FUNCTION: aapiHciLogEnable
 * USE:      Enable/disable HCI logging
 * PARAMS:   enable - on or off?
 * RETURN:   bt_status_t
 * NOTES:
 */
static int aapiHciLogEnable(uint8_t enable)
{
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
    /** BT SSP Reply - Just Works, Numeric Comparison and Passkey
     * passkey shall be zero for BT_SSP_VARIANT_PASSKEY_COMPARISON &
     * BT_SSP_VARIANT_CONSENT
     * For BT_SSP_VARIANT_PASSKEY_ENTRY, if accept==FALSE, then passkey
     * shall be zero */
    //TODO
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
    /** If accept==FALSE, then pin_len and pin_code shall be 0x0 */
    //TODO
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
    //TODO
    return 0;
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
    //TODO
    return 0;
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
    //TODO
    return 0;
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
    //TODO
    return 0;
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
    //TODO
    return 0;
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
    //TODO
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
//    if (!strcmp(profile_id, BT_PROFILE_GATT_ID))
//        return &pGatt;
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
    uint32_t extraSz = sizeof(bt_property_t[num_properties]);
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_ADAPTER_PROPS, extraSz);

    if (!wi)
        return;

    wi->properties.status = status;
    wi->properties.num_properties = num_properties;
    memcpy(wi->properties.properties, properties, extraSz);
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
    uint32_t extraSz = sizeof(bt_property_t[num_properties]);
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_REMOTE_PROPS, extraSz);

    if (!wi)
        return;

    wi->properties.status = status;
    wi->properties.num_properties = num_properties;
    memcpy(&wi->properties.bd_addr, bd_addr, sizeof(wi->properties.bd_addr));
    memcpy(wi->properties.properties, properties, extraSz);
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
    uint32_t extraSz = sizeof(bt_property_t[num_properties]);
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_DEV_DISCOVERED, extraSz);

    if (!wi)
        return;

    wi->properties.num_properties = num_properties;
    memcpy(wi->properties.properties, properties, extraSz);
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
 * PARAMS:   remote_bd_addr - remote device address (a copy is made)
 *           bd_name - remote device name (a copy is made)
 *           devCls - remote device class
 * RETURN:   NONE
 * NOTES:
 */
void aapiPinReqCbk(const bt_bdaddr_t *remote_bd_addr, const bt_bdname_t *bd_name, uint32_t devCls)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_PIN_REQ, 0);

    if (!wi)
        return;

    wi->pinReq.cod = devCls;
    memcpy(&wi->pinReq.remote_bd_addr, remote_bd_addr, sizeof(wi->pinReq.remote_bd_addr));
    memcpy(&wi->pinReq.bd_name, bd_name, sizeof(wi->pinReq.bd_name));
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiSspReqCbk
 * USE:      Notify android about a need for SSP pairing UI
 * PARAMS:   remote_bd_addr - remote device address (a copy is made)
 *           bd_name - remote device name (a copy is made)
 *           devCls - remote device class
 *           pairing_variant - pairing typ requested
 *           pass_key - the passkey to display, if relevant
 * RETURN:   NONE
 * NOTES:
 */
void aapiSspReqCbk(const bt_bdaddr_t *remote_bd_addr, const bt_bdname_t *bd_name, uint32_t devCls, bt_ssp_variant_t pairing_variant, uint32_t pass_key)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_SSP_REQ, 0);

    if (!wi)
        return;

    wi->sspReq.cod = devCls;
    wi->sspReq.pairing_variant = pairing_variant;
    wi->sspReq.pass_key = pass_key;
    memcpy(&wi->sspReq.remote_bd_addr, remote_bd_addr, sizeof(wi->sspReq.remote_bd_addr));
    memcpy(&wi->sspReq.bd_name, bd_name, sizeof(wi->sspReq.bd_name));
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiBondStateChangedCbk
 * USE:      Notify android about bonding changes
 * PARAMS:   status - the atatus
 *           remote_bd_addr - remote device address (a copy is made)
 *           state - what happened to the bond
 * RETURN:   NONE
 * NOTES:
 */
void aapiBondStateChangedCbk(bt_status_t status, const bt_bdaddr_t *remote_bd_addr, bt_bond_state_t state)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_BOND_STATE_CHG, 0);

    if (!wi)
        return;

    wi->stateCbk.status = status;
    wi->stateCbk.state = state;
    memcpy(&wi->stateCbk.remote_bd_addr, remote_bd_addr, sizeof(wi->stateCbk.remote_bd_addr));
    aapiWorkItemEnqueue(wi);
}

/*
 * FUNCTION: aapiAclStateChanged
 * USE:      Notify android about an ACL link being established
 * PARAMS:   status - the atatus
 *           remote_bd_addr - remote device address (a copy is made)
 *           state - what happened to the bond
 * RETURN:   NONE
 * NOTES:
 */
void aapiAclStateChanged(bt_status_t status, const bt_bdaddr_t *remote_bd_addr, bt_acl_state_t state)
{
    struct aapiWorkItem* wi = aapiWorkItemAlloc(WT_ACL_STATE_CHG, 0);

    if (!wi)
        return;

    wi->stateCbk.status = status;
    wi->stateCbk.state = state;
    memcpy(&wi->stateCbk.remote_bd_addr, remote_bd_addr, sizeof(wi->stateCbk.remote_bd_addr));
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


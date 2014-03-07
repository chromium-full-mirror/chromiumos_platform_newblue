#include <hardware/bluetooth.h>
#include <hardware/bt_gatt.h>
#include <hardware/bt_sock.h>
#include <hardware/bt_pan.h>
#include <hardware/bt_av.h>
#include <hardware/bt_hf.h>
#include <hardware/bt_rc.h>
#include <hardware/bt_hh.h>
#include <hardware/bt_hl.h>
#include <stdlib.h>
#include "android.h"
#include "log.h"

static bt_callbacks_t mBtCbks;



#define BT_PROFILE_HANDSFREE_ID "handsfree"
#define BT_PROFILE_ADVANCED_AUDIO_ID "a2dp"
#define BT_PROFILE_HEALTH_ID "health"
#define BT_PROFILE_SOCKETS_ID "socket"
#define BT_PROFILE_HIDHOST_ID "hidhost"
#define BT_PROFILE_PAN_ID "pan"
#define BT_PROFILE_GATT_ID "gatt"
#define BT_PROFILE_AV_RC_ID "avrcp"




/*
 * FUNCTION: btHciLogEnable
 * USE:      Enable/disable HCI logging
 * PARAMS:   enable - on or off?
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btHciLogEnable(uint8_t enable)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btLeTestMode
 * USE:      Send an LE test command
 * PARAMS:   opcode - the opcode
 *           buf - the command params
 *           len - length of said params
 * RETURN:   bt_status_t
 * NOTES:    LE_Receiver_Test, LE_Transmitter_Test, LE_Test_End commands only
 *           Does not require DUT mode enabled (?)
 */
static int btLeTestMode(uint16_t opcode, uint8_t *buf, uint8_t len)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btDutSend
 * USE:      Send any command to the controller
 * PARAMS:   opcode - the opcode
 *           buf - the command params
 *           len - length of said params
 * RETURN:   bt_status_t
 * NOTES:    intended only for vendor test commands, only valid after DUT mode enabled by .dut_mode_configure
 */
static int btDutSend(uint16_t opcode, uint8_t *buf, uint8_t len)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btDutEnable
 * USE:      Enable/disable DUT mode (used for debugging/testing)
 * PARAMS:   enable - on or off?
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btDutEnable(uint8_t enable)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btSspReply
 * USE:      SSP pairing
 * PARAMS:   bd_addr - the peer adress
 *           variant- pairing type being used
 *           accept - pairing was cancelled by user
 *           passkey - entered code
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btSspReply(const bt_bdaddr_t *bd_addr, bt_ssp_variant_t variant, uint8_t accept, uint32_t passkey)
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
 * FUNCTION: btPinReply
 * USE:      Legacy pairing
 * PARAMS:   bd_addr - the peer adress
 *           accept - pairing was cancelled by user
 *           pin_len - length of entered pin
 *           pin_code - entered code
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btPinReply(const bt_bdaddr_t *bd_addr, uint8_t accept, uint8_t pin_len, bt_pin_code_t *pin_code)
{
    /** If accept==FALSE, then pin_len and pin_code shall be 0x0 */
    //TODO
    return 0;
}

/*
 * FUNCTION: btBondCreate
 * USE:      Start a pairing process
 * PARAMS:   bd_addr - the peer adress
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btBondCreate(const bt_bdaddr_t *bd_addr)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btBondRemove
 * USE:      Delete pairing info
 * PARAMS:   bd_addr - the peer adress
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btBondRemove(const bt_bdaddr_t *bd_addr)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btBondCancel
 * USE:      Cancel a pairing process
 * PARAMS:   bd_addr - the peer adress
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btBondCancel(const bt_bdaddr_t *bd_addr)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btDiscoveryStart
 * USE:      Start a discovery
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btDiscoveryStart(void)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btDiscoveryStop
 * USE:      Cancel an ongoing discovery
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btDiscoveryStop(void)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btGetRemoteServices
 * USE:      Find serives at the peer
 * PARAMS:   remote_addr - the device
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btGetRemoteServices(bt_bdaddr_t *remote_addr)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btGetRemoteServiceRecord
 * USE:      Find and retrieve a service record from a peer
 * PARAMS:   remote_addr - the device
 *           uuid - the UUID to search for
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btGetRemoteServiceRecord(bt_bdaddr_t *remote_addr, bt_uuid_t *uuid)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btGetRemoteDevProperties
 * USE:      Get all of a remote device's properties
 * PARAMS:   remote_addr - the device
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btGetRemoteDevProperties(bt_bdaddr_t *remote_addr)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btGetRemoteDevProperty
 * USE:      Get a remote device's property
 * PARAMS:   remote_addr - the device
 *           type - the property to get
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btGetRemoteDevProperty(bt_bdaddr_t *remote_addr, bt_property_type_t type)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btSetRemoteDevProperty
 * USE:      Set a remote device's property
 * PARAMS:   remote_addr - the device
 *           property - the property to set
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btSetRemoteDevProperty(bt_bdaddr_t *remote_addr, const bt_property_t *property)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btSetAdapterProperty
 * USE:      Set a Bluetooth Adapter property
 * PARAMS:   property - the property to set
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btSetAdapterProperty(const bt_property_t *property)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btGetAdapterProperty
 * USE:      Get a Bluetooth Adapter property
 * PARAMS:   type - the property to get
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btGetAdapterProperty(bt_property_type_t type)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btGetAdapterProperties
 * USE:      Get all Bluetooth Adapter properties at init
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btGetAdapterProperties(void)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btEnable
 * USE:      Enable BT
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btEnable(void)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btDisable
 * USE:      Disable BT
 * PARAMS:   NONE
 * RETURN:   bt_status_t
 * NOTES:
 */
static int btDisable(void)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btSockListen
 * USE:      Listen for connections from a peer on a socket
 * PARAMS:   type - tocket type
 *           service_name - service name to advertise
 *           service_uuid - service uuid to advertise
 *           channel - channel number
 *           sock_fd - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
static bt_status_t btSockListen(btsock_type_t type, const char* service_name, const uint8_t* service_uuid, int channel, int* sock_fd, int flags)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btSockConnect
 * USE:      Try to connect to a peer's matching listening socket
 * PARAMS:   bd_addr - peer address
 *           type - tocket type
 *           uuid - service uuid
 *           channel - channel number
 *           sock_fd - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
static bt_status_t btSockConnect(const bt_bdaddr_t *bd_addr, btsock_type_t type, const uint8_t* uuid, int channel, int* sock_fd, int flags)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btGetProfileIface
 * USE:      Get a pointer to a particular profile's interface
 * PARAMS:   profile_id - the profile
 * RETURN:   bt_status_t
 * NOTES:
 */
static const void* btGetProfileIface(const char *profile_id)
{
    static const btsock_interface_t pSockets = {
        .size = sizeof(btsock_interface_t),
        .listen = btSockListen,
        .connect = btSockConnect,
    };

//    if (!strcmp(profile_id, BT_PROFILE_HANDSFREE_ID))
//        return &pHandsfree;
    if (!strcmp(profile_id, BT_PROFILE_SOCKETS_ID))
        return &pSockets;
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
 * FUNCTION: btInit
 * USE:      Init the stack (but not the chip), get callbacks
 * PARAMS:   callbacks - the callbacks to our user
 * RETURN:   bt_status_t
 * NOTES:    only stack-level init here (aka none)
 */
static int btInit(bt_callbacks_t* callbacks)
{
    memcpy(&mBtCbks, callbacks, sizeof(mBtCbks));
    return 0;
}

/*
 * FUNCTION: btCleanup
 * USE:      Cleanup the stack and prepare for unloading
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void btCleanup(void)
{
    /* nothing here */
}

/*
 * FUNCTION: btGetBtIface
 * USE:      Return the bt_interface_t struct full of ways to call us
 * PARAMS:   NONE
 * RETURN:   the bt_interface_t struct
 * NOTES:    no init here
 */
static const bt_interface_t* btGetBtIface(void)
{
    static const bt_interface_t iface = {
        .size = sizeof(bt_interface_t),
        .init = btInit,
        .cleanup = btCleanup,
        .enable = btEnable,
        .disable = btDisable,
        .get_adapter_properties = btGetAdapterProperties,
        .get_adapter_property = btGetAdapterProperty,
        .set_adapter_property = btSetAdapterProperty,
        .get_remote_device_properties = btGetRemoteDevProperties,
        .get_remote_device_property = btGetRemoteDevProperty,
        .set_remote_device_property = btSetRemoteDevProperty,
        .get_remote_service_record = btGetRemoteServiceRecord,
        .get_remote_services = btGetRemoteServices,
        .start_discovery = btDiscoveryStart,
        .cancel_discovery = btDiscoveryStop,
        .create_bond = btBondCreate,
        .remove_bond = btBondRemove,
        .cancel_bond = btBondCancel,
        .pin_reply = btPinReply,
        .ssp_reply = btSspReply,
        .get_profile_interface = btGetProfileIface,
        .dut_mode_configure = btDutEnable,
        .dut_mode_send = btDutSend,
        .le_test_mode = btLeTestMode,
        .config_hci_snoop_log = btHciLogEnable,
    };

    return &iface;
}

/*
 * FUNCTION: btClose
 * USE:      Module close function
 * PARAMS:   device - our bluetooth_device_t
 * RETURN:   0 on success
 * NOTES:    only module-level cleanup here (aka: none)
 */
static int btClose(struct hw_device_t* device)
{
    bluetooth_device_t *btd = (bluetooth_device_t*)device;

    //TODO: cleanup, if any
    return 0;
}

/*
 * FUNCTION: btOpen
 * USE:      Module open function
 * PARAMS:   module - the module object
 *           name - the module name
 *           abstractionP - we store our bluetooth_device_t there
 * RETURN:   0 on success
 * NOTES:    only module-level init here (aka none)
 */
static int btOpen(const struct hw_module_t* module, char const* name, struct hw_device_t** abstractionP)
{
    bluetooth_device_t *btd = (bluetooth_device_t*)calloc(1, sizeof(bluetooth_device_t));

    btd->common.tag = HARDWARE_DEVICE_TAG;
    btd->common.version = 0;
    btd->common.module = (struct hw_module_t*)module; /* casting const away is bad, but sadly such is our API */
    btd->common.close = btClose;
    btd->get_bluetooth_interface = btGetBtIface;

    *abstractionP = (struct hw_device_t*)btd;

    return 0;
}

/* these are needed for android module loader to open us */
static struct hw_module_methods_t mBtMethods = {
    .open = btOpen,
};
extern hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = 0,
    .id = BT_HARDWARE_MODULE_ID,
    .name = "NewBlue stack",
    .author = "dmitrygr@",
    .methods = &mBtMethods,
};

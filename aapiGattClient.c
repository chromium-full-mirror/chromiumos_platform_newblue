#include <string.h>
#include "aapiGattClient.h"
#include "gatt.h"
#include "hci.h"
#include "log.h"
#include "att.h"
#include "bt.h"
#include "mt.h"




static btgatt_client_callbacks_t mCbks;
static pthread_mutex_t mAapiGattCliLock = PTHREAD_MUTEX_INITIALIZER;
static uniq_t mScanId = 0;


/*
 * FUNCTION: aapiGattClientInit
 * USE:      Init the GATT client profile
 * PARAMS:   cbks - callbacks back into java
 * RETURN:   status
 * NOTES:
 */
bt_status_t aapiGattClientInit(const btgatt_client_callbacks_t *cbks)
{
    memcpy(&mCbks, cbks, sizeof(mCbks));


    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientDeinit
 * USE:      Deinit the GATT client profile
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void aapiGattClientDeinit(void)
{
    //TODO
}


/*
 * FUNCTION: aapiGattClientRegisterClient
 * USE:      Registers a GATT client application with the stack
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientRegisterClient(bt_uuid_t *uuid)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientUnregisterClient
 * USE:      Unregister a client application from the stack
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientUnregisterClient(int client_if)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientPrvDeviceDiscovered
 * USE:      Called on each received LE discovery reply
 * PARAMS:   cbkData - unused
 *           addr - the address we got
 *           rssi - the RSSI
 *           replyType - HCI_ADV_TYPE_* 
 *           eir - the received data
 *           eirLen - the length of said data
 * RETURN:   status
 * NOTES:
 */
static void aapiGattClientPrvDeviceDiscovered(void *cbkData, const struct bt_addr *addr, int8_t rssi, uint8_t replyType, const void *eir, uint8_t eirLen)
{
    //XXX: apparently android does nto care about reply type?
    //XXX: device type lost here
    bt_bdaddr_t bd_addr;
    memcpy(bd_addr.address, addr->addr, sizeof(bd_addr.address));

    mCbks.scan_result_cb(&bd_addr, rssi, (void*)eir);

    /* also add it to know devices for future use */
    hciInquiryUpdatePersistDeviceDb(addr, NULL, eir, eirLen);
}

/*
 * FUNCTION: aapiGattClientScan
 * USE:      Start or stop LE device scanning
 * PARAMS:   start - to start or to stop scanning?
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientScan(bool start)
{
    bt_status_t ret = BT_STATUS_SUCCESS;

    pthread_mutex_lock(&mAapiGattCliLock);

    if (start) {
        if (mScanId) {
            logw("Starting LE scan while another already underway. Cancelling old one.\n");
            hciDiscoverLeStop(mScanId);
        }
        mScanId = hciDiscoverLeStart(aapiGattClientPrvDeviceDiscovered, NULL, false, false); //XXX: we default to passive scan using our real address for now
        if (!mScanId) {
            loge("Failed to start LE scan\n");
            ret = BT_STATUS_FAIL;
        }
    } else if (!mScanId)
        logw("Stopping an LE scan while none is running\n");
    else
        hciDiscoverLeStop(mScanId);

    pthread_mutex_unlock(&mAapiGattCliLock);

    return ret;
}

/*
 * FUNCTION: aapiGattClientConnect
 * USE:      Create a connection to a remote LE or dual-mode device
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientConnect(int client_if, const bt_bdaddr_t *bd_addr, bool is_direct, int transport)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientDisconnect
 * USE:      Disconnect a remote device or cancel a pending connection
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientDisconnect(int client_if, const bt_bdaddr_t *bd_addr,
            int conn_id)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientListen
 * USE:      Start or stop advertisements to listen for incoming connections
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientListen(int client_if, bool start)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientRefresh
 * USE:      Clear the attribute cache for a given device
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientRefresh(int client_if, const bt_bdaddr_t *bd_addr)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientSearchService
 * USE:      Enumerate all GATT services on a connected device. Optionally, the results can be filtered for a given UUID.
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientSearchService(int conn_id, bt_uuid_t *filter_uuid)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientGetIncludedService
 * USE:      Enumerate included services for a given service. Set start_incl_srvc_id to NULL to get the first included service.
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientGetIncludedService(int conn_id, btgatt_srvc_id_t *srvc_id, btgatt_srvc_id_t *start_incl_srvc_id)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientGetCharacteristic
 * USE:      Enumerate characteristics for a given service. Set start_char_id to NULL to get the first characteristic.
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientGetCharacteristic(int conn_id, btgatt_srvc_id_t *srvc_id, btgatt_gatt_id_t *start_char_id)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientGetDescriptor
 * USE:      Enumerate descriptors for a given characteristic. Set start_descr_id to NULL to get the first descriptor.
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientGetDescriptor(int conn_id, btgatt_srvc_id_t *srvc_id, btgatt_gatt_id_t *char_id, btgatt_gatt_id_t *start_descr_id)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientReadCharacteristic
 * USE:      Read a characteristic on a remote device
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientReadCharacteristic(int conn_id, btgatt_srvc_id_t *srvc_id, btgatt_gatt_id_t *char_id, int auth_req)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientWriteCharacteristic
 * USE:      Write a remote characteristic
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientWriteCharacteristic(int conn_id, btgatt_srvc_id_t *srvc_id, btgatt_gatt_id_t *char_id, int write_type, int len, int auth_req, char* p_value)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientReadDescriptor
 * USE:      Read the descriptor for a given characteristic
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientReadDescriptor(int conn_id, btgatt_srvc_id_t *srvc_id, btgatt_gatt_id_t *char_id, btgatt_gatt_id_t *descr_id, int auth_req)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientWriteDescriptor
 * USE:      Write a remote descriptor for a given characteristic
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientWriteDescriptor(int conn_id, btgatt_srvc_id_t *srvc_id, btgatt_gatt_id_t *char_id, btgatt_gatt_id_t *descr_id, int write_type, int len, int auth_req, char* p_value)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientExecuteWrite
 * USE:      Execute a prepared write operation
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientExecuteWrite(int conn_id, int execute)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientRegisterForNotification
 * USE:      Register to receive notifications or indications for a given characteristic
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientRegisterForNotification(int client_if, const bt_bdaddr_t *bd_addr, btgatt_srvc_id_t *srvc_id, btgatt_gatt_id_t *char_id)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientDeregisterForNotification
 * USE:      Deregister a previous request for notifications/indications
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientDeregisterForNotification(int client_if, const bt_bdaddr_t *bd_addr, btgatt_srvc_id_t *srvc_id, btgatt_gatt_id_t *char_id)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientReadRemoteRssi
 * USE:      Request RSSI for a given remote device
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientReadRemoteRssi(int client_if, const bt_bdaddr_t *bd_addr)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientScanFilterParamSetup
 * USE:      Setup scan filter params
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientScanFilterParamSetup(int client_if, int action, int filt_index, int feat_seln, int list_logic_type, int filt_logic_type, int rssi_high_thres, int rssi_low_thres, int dely_mode, int found_timeout, int lost_timeout, int found_timeout_cnt)
{
    //todo
    return BT_STATUS_SUCCESS;
}


/*
 * FUNCTION: aapiGattClientScanFilterAddRemove
 * USE:      Configure a scan filter condition 
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientScanFilterAddRemove(int client_if, int action, int filt_type, int filt_index, int company_id, int company_id_mask, const bt_uuid_t *p_uuid, const bt_uuid_t *p_uuid_mask, const bt_bdaddr_t *bd_addr, char addr_type, int data_len, char* p_data, int mask_len, char* p_mask)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientScanFilterClear
 * USE:      Clear all scan filter conditions for specific filter index
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientScanFilterClear(int client_if, int filt_index)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientScanFilterEnable
 * USE:      Enable / disable scan filter feature
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientScanFilterEnable(int client_if, bool enable)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientGetDeviceType
 * USE:      Determine the type of the remote device (LE, BR/EDR, Dual-mode)
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
int aapiGattClientGetDeviceType(const bt_bdaddr_t *bd_addr)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientSetAdvData
 * USE:      Set the advertising data or scan response data
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientSetAdvData(int client_if, bool set_scan_rsp, bool include_name, bool include_txpower, int min_interval, int max_interval, int appearance, uint16_t manufacturer_len, char* manufacturer_data, uint16_t service_data_len, char* service_data, uint16_t service_uuid_len, char* service_uuid)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientConfigureMtu
 * USE:      Configure the MTU for a given connection
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientConfigureMtu(int conn_id, int mtu)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientReqConnParamUpdate
 * USE:      Request a connection parameter update
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientReqConnParamUpdate(const bt_bdaddr_t *bd_addr, int min_interval, int max_interval, int latency, int timeout)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientSetScanParams
 * USE:      Sets the LE scan interval and window in units of N*0.625 msec
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientSetScanParams(int scan_interval, int scan_window)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientMultiAdvEnable
 * USE:      Setup the parameters as per spec, user manual specified values and enable multi ADV
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientMultiAdvEnable(int client_if, int min_interval,int max_interval,int adv_type, int chnl_map, int tx_power, int timeout_s)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientMultiAdvUpdate
 * USE:      Update the parameters as per spec, user manual specified values and restart multi ADV
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientMultiAdvUpdate(int client_if, int min_interval,int max_interval,int adv_type, int chnl_map, int tx_power, int timeout_s)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientMultiAdvSetInstData
 * USE:      Setup the data for the specified instance
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientMultiAdvSetInstData(int client_if, bool set_scan_rsp, bool include_name, bool incl_txpower, int appearance, int manufacturer_len, char* manufacturer_data, int service_data_len, char* service_data, int service_uuid_len, char* service_uuid)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientMultiAdvDisable
 * USE:      Disable the multi adv instance
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientMultiAdvDisable(int client_if)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientBatchScanCfgStorage
 * USE:      Configure the batchscan storage
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientBatchScanCfgStorage(int client_if, int batch_scan_full_max, int batch_scan_trunc_max, int batch_scan_notify_threshold)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientBatchScanEnable
 * USE:      Enable batchscan
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientBatchScanEnable(int client_if, int scan_mode, int scan_interval, int scan_window, int addr_type, int discard_rule)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientBatchScanDisable
 * USE:      Disable batchscan
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientBatchScanDisable(int client_if)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientBatchScanReadReports
 * USE:      Read out batchscan reports
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientBatchScanReadReports(int client_if, int scan_mode)
{
    //todo
    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattClientTest
 * USE:      Test mode interface
 * PARAMS:   ??
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientTest(int command, btgatt_test_params_t* params)
{
    //todo
    return BT_STATUS_SUCCESS;
}



/*
 * FUNCTION: aapiGattClientGetProfileIface
 * USE:      Called to get profile pointer to the GATT client profile
 * PARAMS:   NONE
 * RETURN:   pointer to profile struct
 * NOTES:
 */
void *aapiGattClientGetProfileIface(void)
{
    static const btgatt_client_interface_t iface = {
        .register_client = aapiGattClientRegisterClient,
        .unregister_client = aapiGattClientUnregisterClient,
        .scan = aapiGattClientScan,
        .connect = aapiGattClientConnect,
        .disconnect = aapiGattClientDisconnect,
        .listen = aapiGattClientListen,
        .refresh = aapiGattClientRefresh,
        .search_service = aapiGattClientSearchService,
        .get_included_service = aapiGattClientGetIncludedService,
        .get_characteristic = aapiGattClientGetCharacteristic,
        .get_descriptor = aapiGattClientGetDescriptor,
        .read_characteristic = aapiGattClientReadCharacteristic,
        .write_characteristic = aapiGattClientWriteCharacteristic,
	.read_descriptor = aapiGattClientReadDescriptor,
        .write_descriptor = aapiGattClientWriteDescriptor,
        .execute_write = aapiGattClientExecuteWrite,
        .register_for_notification = aapiGattClientRegisterForNotification,
        .deregister_for_notification = aapiGattClientDeregisterForNotification,
        .read_remote_rssi = aapiGattClientReadRemoteRssi,
        .scan_filter_param_setup = aapiGattClientScanFilterParamSetup,
        .scan_filter_add_remove = aapiGattClientScanFilterAddRemove,
        .scan_filter_clear = aapiGattClientScanFilterClear,
        .scan_filter_enable = aapiGattClientScanFilterEnable,
        .get_device_type = aapiGattClientGetDeviceType,
        .set_adv_data = aapiGattClientSetAdvData,
        .configure_mtu = aapiGattClientConfigureMtu,
        .conn_parameter_update = aapiGattClientReqConnParamUpdate,
        .set_scan_parameters = aapiGattClientSetScanParams,
        .multi_adv_enable = aapiGattClientMultiAdvEnable,
        .multi_adv_update = aapiGattClientMultiAdvUpdate,
        .multi_adv_set_inst_data = aapiGattClientMultiAdvSetInstData,
        .multi_adv_disable = aapiGattClientMultiAdvDisable,
        .batchscan_cfg_storage = aapiGattClientBatchScanCfgStorage,
        .batchscan_enb_batch_scan = aapiGattClientBatchScanEnable,
        .batchscan_dis_batch_scan = aapiGattClientBatchScanDisable,
        .batchscan_read_reports = aapiGattClientBatchScanReadReports,
        .test_command = aapiGattClientTest,
    };


    return (void*)&iface;
}


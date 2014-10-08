#include <string.h>
#include "aapiGattClient.h"
#include "workQueue.h"
#include "config.h"
#include "gatt.h"
#include "hci.h"
#include "log.h"
#include "att.h"
#include "bt.h"
#include "mt.h"



#define GATT_CONN_STATE_OPENING       0 /* open is in progress at lower layers */
#define GATT_CONN_STATE_RUNNING       1 /* all is good */
#define GATT_CONN_STATE_TEARDOWN      2 /* on its way down */

#define GATT_CLICON_STATE_OPENWAIT    0 /* waiting for connection to open */
#define GATT_CLICON_STATE_RUNNING     1 /* all is good */
#define GATT_CLICON_STATE_TEARDOWN    2 /* on its way down */

#define WORK_TYPER_NOTIF_CONN_OPEN    0

struct aapiGattClientWorkItem {
    uint8_t type;
    union {
        struct {
            int conn_id;
            int status;
            int client_if;
            struct bt_addr addr;
        } connOpen;

    };
};

struct aapiGattClient {
    struct aapiGattClient         *next;
    struct aapiGattClient         *prev;
    int                            client_if;
    bt_uuid_t                      andrUuid;
};

struct aapiGattConn {
    struct aapiGattConn           *prev;
    struct aapiGattConn           *next;
    l2c_handle_t                   conn;
    struct bt_addr                 addr;
    uint8_t                        state; /* GATT_CONN_STATE_* */
};

struct aapiGattClientConn { /* many-to-many table of the aapiGattClient x aapiGattConn association */
    struct aapiGattClientConn     *next;
    struct aapiGattClientConn     *prev;
    struct aapiGattClient         *client;
    struct aapiGattConn           *conn;
    int                            conn_id;
    uint8_t                        state; /* GATT_CLICON_STATE_* */
};

static btgatt_client_callbacks_t mCbks;
static pthread_mutex_t mAapiGattCliLock = PTHREAD_MUTEX_INITIALIZER;
static struct aapiGattClientConn *mClientConns;
static struct aapiGattClient *mClients;
static struct aapiGattConn *mConns;
static struct workQueue *mWorkQ;
static pthread_t mWorkThread;
static uniq_t mScanId;



/*
 * FUNCTION: aapiGattClientFindStructByRef
 * USE:      Find a aapiGattClient struct by reference number
 * PARAMS:   client_if - the reference
 * RETURN:   the aapiGattClient struct or NULL if not found
 * NOTES:    call with mAapiGattCliLock held
 */
static struct aapiGattClient* aapiGattClientFindStructByRef(int client_if)
{
    struct aapiGattClient *ret = mClients;

    while (ret && ret->client_if != client_if)
        ret = ret->next;

    return ret;
}

/*
 * FUNCTION: aapiGattClientFindStructByUuid
 * USE:      Find a aapiGattClient struct by android-style uuid
 * PARAMS:   andrUuid - the uuid
 * RETURN:   the aapiGattClient struct or NULL if not found
 * NOTES:    call with mAapiGattCliLock held
 */
static struct aapiGattClient* aapiGattClientFindStructByUuid(const bt_uuid_t *andrUuid)
{
    struct aapiGattClient *ret = mClients;

    while (ret && memcmp(&ret->andrUuid, andrUuid, sizeof(ret->andrUuid)))
        ret = ret->next;

    return ret;
}

/*
 * FUNCTION: aapiGattClientConnFind
 * USE:      Find a aapiGattConn struct by bt address (assume it has already been privacy-resolved)
 * PARAMS:   addr - the address in question
 * RETURN:   the aapiGattClientConnFind struct or NULL if not found
 * NOTES:    call with mAapiGattCliLock held
 */
static struct aapiGattConn* aapiGattClientConnFind(const struct bt_addr *addr)
{
    struct aapiGattConn *ret = mConns;

    while (ret && memcmp(&ret->addr, addr, sizeof(ret->addr)))
        ret = ret->next;

    return ret;
}

/*
 * FUNCTION: aapiGattClientFindClientConnection
 * USE:      Find a aapiGattClientConn struct by client and connection
 * PARAMS:   cli - the client struct
 *           conn - the connection struct
 * RETURN:   the aapiGattClientConn struct or NULL if not found
 * NOTES:    call with mAapiGattCliLock held
 */
static struct aapiGattClientConn* aapiGattClientFindClientConnection(struct aapiGattClient *cli, struct aapiGattConn *conn)
{
    struct aapiGattClientConn *ret = mClientConns;

    while (ret && (ret->client != cli || ret->conn != conn))
        ret = ret->next;

    return ret;
}

/*
 * FUNCTION: aapiGattClientFindClientConnectionByConnId
 * USE:      Find a aapiGattClientConn struct by connection id
 * PARAMS:   conn_id - the connection id
 * RETURN:   the aapiGattClientConn struct or NULL if not found
 * NOTES:    I am not sure if conn_id needs to be unique per client_if or globally.
 *           Since I do not forsee 2 billion connections, here they are globally
 *           unique. Why not?
 */
static struct aapiGattClientConn* aapiGattClientFindClientConnectionByConnId(int conn_id)
{
    struct aapiGattClientConn *ret = mClientConns;

    while (ret && ret->conn_id)
        ret = ret->next;

    return ret;
}

/*
 * FUNCTION: aapiGattClientWorkerThread
 * USE:      Aapi.GATT.Client Worker thread
 * PARAMS:   unused - unused
 * RETURN:   ignored
 * NOTES:
 */
static void* aapiGattClientWorkerThread(void *unused)
{
    struct aapiGattClientWorkItem *wi;
    bt_bdaddr_t bda;
    int status;

    pthread_setname_np(pthread_self(), "aapi_gatt_client_worker");

    while(1) {
        status = workQueueGet(mWorkQ, (void**)&wi);

        /* The only one thing we use the work queue status for - exiting */
        if (status)
            break;

        switch (wi->type) {
        case WORK_TYPER_NOTIF_CONN_OPEN:
            memcpy(bda.address, wi->connOpen.addr.addr, sizeof(bda.address));
            mCbks.open_cb(wi->connOpen.conn_id, wi->connOpen.status, wi->connOpen.client_if, &bda);
            break; 

        default:
            loge("Unknown work type %d\n", wi->type);
            break;
        }
        free(wi);
    }

    return NULL;
}

/*
 * FUNCTION: aapiGattClientScheduleConnOpenNotif
 * USE:      Schedule a call to notify that a connection's been opened
 * PARAMS:   cliCon - the client-connection structure which tells us whom to notify and what of
 *           status - status to send
 * RETURN:   NONE
 * NOTES:    errors are logged and ignored since there isn't much else to do about them
 */
static void aapiGattClientScheduleConnOpenNotif(struct aapiGattClientConn *cliCon, int status)
{
    struct aapiGattClientWorkItem *wi = (struct aapiGattClientWorkItem*)malloc(sizeof(struct aapiGattClientWorkItem));

    if (!wi)
        loge("Failed to allocate gatt.client work\n");
    else {
        wi->type = WORK_TYPER_NOTIF_CONN_OPEN;
        wi->connOpen.conn_id = cliCon->conn_id;
        wi->connOpen.client_if = cliCon->client->client_if;
        wi->connOpen.status = status;
        memcpy(&wi->connOpen.addr, &cliCon->conn->addr, sizeof(struct bt_addr));
        if (workQueuePut(mWorkQ, wi))
            return;
        loge("Failed to enqueue gatt.client work\n");
        free(wi);
    }
}


/*
 * FUNCTION: aapiGatClientWorkQItemFreeCbk
 * USE:      Used to free iteam in workquque cleanup
 * PARAMS:   workItem - an item to free
 * RETURN:   NONE
 * NOTES:
 */
static void aapiGatClientWorkQItemFreeCbk(void *workItem)
{
    struct aapiGattClientWorkItem *wi = (struct aapiGattClientWorkItem*)workItem;

    free(wi);
}

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
    mClients = NULL;
    mConns = NULL;
    mClientConns = NULL;
    mScanId = 0;

    mWorkQ = workQueueAlloc(GATT_CLI_WORK_Q_SIZE);
    if (!mWorkQ) {
        loge("Failed to alloc gatt client workqueue\n");
        return BT_STATUS_FAIL;
    }

    if (!pthread_create(&mWorkThread, NULL, aapiGattClientWorkerThread, NULL))
        return BT_STATUS_SUCCESS;

    workQueueFree(mWorkQ, aapiGatClientWorkQItemFreeCbk);
    return BT_STATUS_FAIL;
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
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorkThread, NULL);
    workQueueFree(mWorkQ, aapiGatClientWorkQItemFreeCbk);
}


/*
 * FUNCTION: aapiGattClientRegisterClient
 * USE:      Registers a GATT client application with the stack
 * PARAMS:   andrUuid - the unique Id of this client
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientRegisterClient(bt_uuid_t *andrUuid)
{
    bt_status_t ret = BT_STATUS_FAIL;
    int ref = 0;

    pthread_mutex_lock(&mAapiGattCliLock);
    if (aapiGattClientFindStructByUuid(andrUuid))
        loge("Refusing to create identical GATT client\n");
    else {
        struct aapiGattClient *cli = (struct aapiGattClient*)calloc(1, sizeof(struct aapiGattClient));
        if (!cli) {
            loge("Failed to allocate GATT client\n");
            ret = BT_STATUS_NOMEM;
        } else {

            /* grab a free reference number */
            cli->client_if = mClients ? (mClients->client_if + 1) : 1;
            while (aapiGattClientFindStructByRef(cli->client_if))
                cli->client_if++;

            /* link it in */
            cli->next = mClients;
            if (mClients)
                mClients->prev = cli;
            mClients = cli;

            /* save uuid */
            memcpy(&cli->andrUuid, andrUuid, sizeof(cli->andrUuid));

            /* prepare to announce success */
            ret = BT_STATUS_SUCCESS;
            ref = cli->client_if;
        }
    }
    pthread_mutex_unlock(&mAapiGattCliLock);

    mCbks.register_client_cb(ret, ref, andrUuid);
    return ret;
}

/*
 * FUNCTION: aapiGattClientUnregisterClient
 * USE:      Unregister a client application from the stack
 * PARAMS:   client_if - the client identifier
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientUnregisterClient(int client_if)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattClientConn *cc;
    struct aapiGattClient *cli;

    pthread_mutex_lock(&mAapiGattCliLock);
    cli = aapiGattClientFindStructByRef(client_if);
    if (!cli)
        logw("Client not found while attempting to unregister\n");
    else {
        uint32_t i;

        if (cli->next)
            cli->next->prev = cli->prev;
        if (cli->prev)
            cli->prev->next = cli->next;
        else
            mClients = cli->next;

        //TODO: more cleanup work here as needed        

        free(cli);
        ret = BT_STATUS_SUCCESS;

        /* terminate all connections with prejudice */
        cc = mClientConns;
        while (cc) {
            struct aapiGattClientConn *next = cc->next;
            if (cc->client == cli)
                aapiGattClientConnTerminate(cc, false/*do not notify anyone since there is nothing to notify about */);
            cc = next;
        }
    }
    pthread_mutex_unlock(&mAapiGattCliLock);

    return ret;
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
    //XXX: apparently android does not care about reply type?
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
    else {
        mScanId = 0;
        hciDiscoverLeStop(mScanId);
    }
    pthread_mutex_unlock(&mAapiGattCliLock);

    return ret;
}

/*
 * FUNCTION: aapiGattClientConnect
 * USE:      Create a connection to a remote LE or dual-mode device
 * PARAMS:   client_if - the client identifier
 *           bd_addr - whom to connect to
 *           is_direct - ? ? ?
 *           transport - GATT_TRANSPORT_*
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattClientConnect(int client_if, const bt_bdaddr_t *bd_addr, bool is_direct, int transport)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattClientConn *cliCon;
    struct aapiGattClient *cli;
    struct aapiGattConn *conn;
    struct bt_addr addr;
    int conn_id;


    if (!is_direct) {
        loge("Non-direct gatt connect not supported yet\n");
        /* what does it mean anyways? */
        return BT_STATUS_FAIL;
    }

    pthread_mutex_lock(&mAapiGattCliLock);

    cli = aapiGattClientFindStructByRef(client_if);
    if (!cli) {
        loge("Invalid client id seen\n");
        goto out;
    }

    memcpy(addr.addr, bd_addr->address, BT_MAC_LEN);
    addr.type = (transport == BT_TRANSPORT_LE) ? BT_ADDR_TYPE_LE_RANDOM : BT_ADDR_TYPE_EDR; /* random only for now */
    //TODO: privacy mode resolution


    conn = aapiGattClientConnFind(&addr);
    if (!conn) {
        logd("Connection to "ADDRFMT" not found - creating a new one\n", ADDRCONV(addr));
        conn = aapiGattClientConnCreate(&addr);
        if (!conn) {
            logw("Connection to "ADDRFMT" failed suspiciously quickly\n", ADDRCONV(addr));
            goto out;
        }
    }

    if (conn->state == GATT_CONN_STATE_TEARDOWN) {
        logi("Tried to form gatt connection on a link that is going down. Sorry, you're SOL\n");
        goto out;
    }

    cliCon = aapiGattClientFindClientConnection(cli, conn);
    if (cliCon) {
        logw("Rejecting attemt to create duplicate connection from same client\n");
        goto out;
    }

    /* allocate a cliCon struct now and link it in */
    cliCon = (struct aapiGattClientConn*)malloc(sizeof(struct aapiGattClientConn));
    if (!cliCon) {
        loge("Memory error allocating client connection struct. Sorry\n");
        goto out;
    }

    /* grab a non-zero connection id */
    cliCon->conn_id = mClientConns ? mClientConns->conn_id : 1;
    while (aapiGattClientFindClientConnectionByConnId(cliCon->conn_id) || !cliCon->conn_id)
        cliCon->conn_id++;

    /* init other fields */
    cliCon->conn = conn;
    cliCon->client = cli;

    /* link it in */
    cliCon->prev = NULL;
    cliCon->next = mClientConns;
    if (mClientConns)
        mClientConns->prev = cliCon;
    mClientConns = cliCon;


    if (conn->state == GATT_CONN_STATE_OPENING) {
        cliCon->state = GATT_CLICON_STATE_OPENWAIT;
        ret = BT_STATUS_SUCCESS;
        goto out;
    }

    aapiGattClientScheduleConnOpenNotif(cliCon, 0);
    ret = BT_STATUS_SUCCESS;

out:
    pthread_mutex_unlock(&mAapiGattCliLock);
    return ret;
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


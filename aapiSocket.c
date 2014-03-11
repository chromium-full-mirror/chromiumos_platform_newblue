#include <hardware/bluetooth.h>
#include <hardware/bt_sock.h>
#include "aapiSocket.h"
#include "rfcomm.h"
#include "sdp.h"
#include "log.h"


/*
 * FUNCTION: aapiSockAddSdpRecord
 * USE:      Add an SDP record for this service
 * PARAMS:   serviceUuid - the uuid
 *           rfcommCh - the rfcomm channel number
 *           name - the name to advertise
 * RETURN:   handle or 0 on error
 * NOTES:
 */
static uint32_t aapiSockAddSdpRecord(const uint8_t* serviceUuid, uint8_t rfcommCh, const char* name)
{
    uint8_t *record, *t;
    uint32_t ret, nameLen = name ? strlen(name) : 0, recLen;
    static const uint8_t preUuidData[] = {
        /* service class ID list */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_CLS_ID_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 17, SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_16)};

    /* here go 16 bbytes of uuid */

    static const uint8_t preChannelData[] = {
        /* ServiceId */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_ID),
        SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_SVC_CLASS_SPP),
        /* ProtocolDescriptorList */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_PROTOCOL_DESCR_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 15,
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_L2CAP),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(PSM_RFCOMM),
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 5,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_RFCOMM),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_1)};

    /* here goes one byte channel number */

    static const uint8_t preNameData[] = {
        /* browse group list */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_BROWSE_GRP_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_BROWSE_GROUP_ID_PUBLIC),
        /* name */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SPP_NAME),
        SDP_ITEM_DESC(SDP_TYPE_TEXT, SDP_SZ_u8)};

    /* here goes one-byte name length and then the name */


    if (nameLen > 255) {
        logw("Truncating RFCOMM advertised name length\n");
        nameLen = 255;
    }
    recLen = sizeof(preUuidData) + sizeof(preChannelData) + sizeof(preNameData) + 16 + 1 + 1 + nameLen;
    record = malloc(recLen);
    if (!record)
        return 0;

    t = record;
    memcpy(t, preUuidData, sizeof(preUuidData));
    t += sizeof(preUuidData);
    memcpy(t, serviceUuid, 16);
    t += 16;
    memcpy(t, preChannelData, sizeof(preChannelData));
    t += sizeof(preChannelData);
    *t++ = rfcommCh;
    memcpy(t, preNameData, sizeof(preNameData));
    t += sizeof(preNameData);
    *t++ = nameLen;
    memcpy(t, name, nameLen);

    ret = sdpServiceDescriptorAdd(record, recLen);
    if (ret)
        return ret;

    free(record);
    return 0;
}

/*
 * FUNCTION: aapiSockListen
 * USE:      Listen for connections from a peer on a socket
 * PARAMS:   type - tocket type
 *           serviceName - service name to advertise
 *           serviceUuid - service uuid to advertise
 *           channel - channel number
 *           sockFdP - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
static bt_status_t aapiSockListen(btsock_type_t type, const char* serviceName, const uint8_t* serviceUuid, int channel, int* sockFdP, int flags)
{
    logd("AAPI.sock: listen(%u,'%s',%d,%u)\n", type, serviceName, channel, flags);
    //TODO
    return BT_STATUS_FAIL;
}

/*
 * FUNCTION: aapiSockConnect
 * USE:      Try to connect to a peer's matching listening socket
 * PARAMS:   bd_addr - peer address
 *           type - tocket type
 *           uuid - service uuid
 *           channel - channel number
 *           sockFdP - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
static bt_status_t aapiSockConnect(const bt_bdaddr_t *bd_addr, btsock_type_t type, const uint8_t* uuid, int channel, int* sockFdP, int flags)
{
    logd("AAPI.sock: connect(%02X:%02X:%02X:%02X:%02X:%02X,%u,%d,%u)\n",
         bd_addr->address[5], bd_addr->address[4], bd_addr->address[3], bd_addr->address[2], bd_addr->address[1], bd_addr->address[0],
         type, channel, flags);
    //TODO
    return BT_STATUS_FAIL;
}


/*
 * FUNCTION: aapiGetProfileIfaceSockets
 * USE:      Try to connect to a peer's matching listening socket
 * PARAMS:   bd_addr - peer address
 *           type - tocket type
 *           uuid - service uuid
 *           channel - channel number
 *           sockFdP - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
const void* aapiGetProfileIfaceSockets(void)
{
    static const btsock_interface_t sockIface = {
        .size = sizeof(btsock_interface_t),
        .listen = aapiSockListen,
        .connect = aapiSockConnect,
    };

    return &sockIface;
}

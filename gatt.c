#include "l2cap.h"
#include "gatt.h"
#include "sdp.h"



/*
 * FUNCTION: gattSdpInit
 * USE:      Called to add GATT to SDP
 * PARAMS:   genericAttrProfileHandle - start handle for generic attribute profile
 *           genericAttrProfileLen - length of generic attribute profile
 * RETURN:   sdp handle or 0 on error
 * NOTES:
 */
static uint32_t gattSdpInit(uint16_t genericAttrProfileHandle, uint16_t genericAttrProfileLen)
{
    uint16_t genericAttrProfileEnd = genericAttrProfileHandle + genericAttrProfileLen - 1;

    uint8_t sdpDescrGatt[] = {
        //service class ID list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_CLS_ID_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_SVC_CLASS_GATT),

        //ProtocolDescriptorList
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_PROTOCOL_DESCR_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 19,
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_L2CAP),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(GATT_PSM),
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 9,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_ATT),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(genericAttrProfileHandle),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(genericAttrProfileEnd),

        //browse group list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_BROWSE_GRP_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_BROWSE_GROUP_ID_PUBLIC),

    };

    return sdpServiceDescriptorAdd(sdpDescrGatt, sizeof(sdpDescrGatt));
}


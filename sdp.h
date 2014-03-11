#ifndef _SDP_H_
#define _SDP_H_

#include "types.h"


#define SDP_TYPE_NIL                     0
#define SDP_TYPE_UINT                    1
#define SDP_TYPE_SINT                    2
#define SDP_TYPE_UUID                    3
#define SDP_TYPE_TEXT                    4
#define SDP_TYPE_BOOL                    5
#define SDP_TYPE_ARRAY                   6        //"data element sequence"
#define SDP_TYPE_OR_LIST                 7        //"data element alternative" - pick one of these
#define SDP_TYPE_URL                     8

#define SDP_SZ_NIL                       0
#define SDP_SZ_1                         0
#define SDP_SZ_2                         1
#define SDP_SZ_4                         2
#define SDP_SZ_8                         3
#define SDP_SZ_16                        4
#define SDP_SZ_u8                        5
#define SDP_SZ_u16                       6
#define SDP_SZ_u32                       7

#define SDP_ITEM_DESC_TYPE_MASK          0xF8
#define SDP_ITEM_DESC_TYPE_SHIFT         3
#define SDP_ITEM_DESC_SIZE_MASK          0x07
#define SDP_ITEM_DESC_SIZE_SHIFT         0
#define SDP_ITEM_DESC(type, size)        ((((type) << SDP_ITEM_DESC_TYPE_SHIFT) & SDP_ITEM_DESC_TYPE_MASK) | \
                                          (((size) << SDP_ITEM_DESC_SIZE_SHIFT) & SDP_ITEM_DESC_SIZE_MASK))
#define SDP_ITEM_GET_TYPE(desc)          (((desc) & SDP_ITEM_DESC_TYPE_MASK) >> SDP_ITEM_DESC_TYPE_SHIFT)
#define SDP_ITEM_GET_SIZE(desc)          (((desc) & SDP_ITEM_DESC_SIZE_MASK) >> SDP_ITEM_DESC_SIZE_SHIFT)


/* useful attribute IDs */
#define SDP_ATTR_HANDLE                  0x0000
#define SDP_ATTR_SVC_CLS_ID_LIST         0x0001
#define SDP_ATTR_SVC_ID                  0x0003
#define SDP_ATTR_PROTOCOL_DESCR_LIST     0x0004
#define SDP_ATTR_BROWSE_GRP_LIST         0x0005

/* other useful numbers */
#define SDP_BROWSE_GROUP_ID_PUBLIC       0x1002
#define SDP_PROTO_L2CAP                  0x0100 /* expects param uint16_t psm */
#define SDP_PROTO_RFCOMM                 0x0003 /* expects param uint8_t dlci */
#define SDP_SVC_CLASS_SPP                0x1101
#define SDP_ATTR_SPP_NAME                0x0100

#define SDP_U16(v)                       (uint8_t)((v) >> 8),(uint8_t)(v)
#define SDP_U32(v)                       (uint8_t)((v) >> 24),(uint8_t)((v) >> 26),(uint8_t)((v) >> 8),(uint8_t)(v)


/* init the profile */
bool sdpRegisterProfile(void);


/* add or delete services - we assume descriptors are in valid format (attrs in rising order, etc) */
uint32_t sdpServiceDescriptorAdd(const void *svc, uint32_t len); //a copy will be made -> handle
bool sdpServiceDescriptorDel(uint32_t handle);



#endif


#ifndef _PERSIST_H_
#define _PERSIST_H_



#include "types.h"

/* names */
#define PERS_NAME_DEVICE_NAME            0x00000001 /* type: PERS_TYPE_BYTE_ARRAY, size: 0..HCI_DEV_NAME_LEN */
#define PERS_NAME_DISC_TIMEOUT           0x00000002 /* type: PERS_TYPE_UINT, size: 1 */
#define PERS_NAME_DEV_CLS                0x00000003 /* type: PERS_TYPE_UINT, size: 4 */
#define PERS_NAME_KEYS                   0x00000004 /* type: PERS_TYPE_ARRAY of {PERS_NAME_KEY_TYPE, PERS_NAME_KEY_VAL} */
#define PERS_NAME_KEY_TYPE               0x00000005 /* type: PERS_TYPE_UINT, size: 1 */
#define PERS_NAME_KEY_VAL                0x00000006 /* type: PER_TYPE_BYTE_ARRAY */
#define PERS_NAME_BONDED_DEVICES         0x00000007 /* type: PERS_TYPE_ARRAY of {PERS_NAME_DEVICE_NAME, PERS_NAME_BT_ADDR, PERS_NAME_KEYS} */
#define PERS_NAME_BT_ADDR                0x00000008 /* type: PERS_TYPE_BT_ADDR */     

/* types */
#define PERS_TYPE_UINT                   0x00000001
#define PERS_TYPE_SINT                   0x00000002
#define PERS_TYPE_BYTE_ARRAY             0x00000003
#define PERS_TYPE_BT_ADDR                0x00000004
#define PERS_TYPE_ARRAY                  0x00000005



#endif


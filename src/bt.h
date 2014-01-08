#ifndef _BT_H_
#define _BT_H_

#include "types.h"

struct bt_addr {
    uint8_t addr[6];
    uint8_t type;
};
#define BT_ADDR_TYPE_EDR         0
#define BT_ADDR_TYPE_LE_PUBLIC   1
#define BT_ADDR_TYPE_LE_RANDOM   2
#define BT_ADDR_TYPE_NUM         3

#define BT_ADDR_IS_EDR(addr)     ((addr).type == BT_ADDR_TYPE_EDR)
#define BT_ADDR_IS_LE(addr)      ((addr).type != BT_ADDR_TYPE_EDR)

#endif


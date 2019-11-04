#include "bt.h"

#include <stdio.h>

/* FUNCTION:  ba2str
 * USE:       Convert a bt_addr to string
 * PARAMS:    bdaddr  - address structure
 *            buf     - output string buffer where bt_addr will be written
 *            len     - length of output buffer
 * RETURN:    Number of bytes written to output buffer
 * NOTES:     Address will be uppercase */
int ba2str(const struct bt_addr* bdaddr, char* buf, size_t len) {
    const uint8_t* addr = &(bdaddr->addr[0]);
    return snprintf(buf, len, "%2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X", addr[5],
                    addr[4], addr[3], addr[2], addr[1], addr[0]);
}

/* FUNCTION:  ba2strlc
 * USE:       Convert a bt_addr to lowercase string
 * PARAMS:    bdaddr  - address structure
 *            buf     - output string buffer where bt_addr will be written
 *            len     - length of output buffer
 * RETURN:    Number of bytes written to output buffer
 * NOTES:     Address will be lowercase to match kernel formatting %pMR */
int ba2strlc(const struct bt_addr* bdaddr, char* buf, size_t len) {
    const uint8_t* addr = &(bdaddr->addr[0]);
    return snprintf(buf, len, "%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x", addr[5],
                    addr[4], addr[3], addr[2], addr[1], addr[0]);
}

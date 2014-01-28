#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include "vendorLib.h"
#include "config.h"
#include "l2cap.h"
#include "util.h"
#include "uniq.h"
#include "hci.h"
#include "log.h"





/*
 * FUNCTION: hciUp
 * USE:      Bring up HCI (comms to bt chip)
 * PARAMS:   NONE
 * RETURN:   true on success, false else
 * NOTES:    may take a while
 */
bool hciUp(void)
{
    if (!vendorOpen()) {
        loge("Failed to open vendor lib\n");
        goto out_vendor_lib;
    }

    //more init here

    return true;

out_vendor_lib:
    return false;
}


/*
 * FUNCTION: hciDown
 * USE:      Bring down HCI (comms to bt chip)
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    may take a while, some shutdown actions may go
 *           on after this (in higher layers)
 */
void hciDown(void)
{
    //todo: this
}

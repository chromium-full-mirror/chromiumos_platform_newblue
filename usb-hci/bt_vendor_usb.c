/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bt_vendor_lib.h"
#include "usb.h"

#include <stdio.h>

static const bt_vendor_callbacks_t* sCallbacks;


static int init(const bt_vendor_callbacks_t* cb, unsigned char *local_bdaddr)
{
    fprintf(stderr, "bt-vendor : init\n");

    if (init_usb()) {
        fprintf(stderr, "init_usb failed\n");
        return -1;
    }

    sCallbacks = cb;
    return 0;
}

static int op(bt_vendor_opcode_t opcode, void *param)
{
    switch (opcode) {
        case BT_VND_OP_POWER_CTRL:
            //printf("BT_VND_OP_POWER_CTRL\n");
            return 0;

        case BT_VND_OP_FW_CFG:
            //printf("BT_VND_OP_FW_CFG\n");
            sCallbacks->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);
            return 0;

        case BT_VND_OP_SCO_CFG:
            //printf("BT_VND_OP_SCO_CFG\n");
            sCallbacks->scocfg_cb(BT_VND_OP_RESULT_SUCCESS);
            return 0;

        case BT_VND_OP_USERIAL_OPEN:
            fprintf(stderr, "BT_VND_OP_USERIAL_OPEN\n");
            return usb_serial_open(param);

        case BT_VND_OP_USERIAL_CLOSE:
            fprintf(stderr, "BT_VND_OP_USERIAL_CLOSE\n");
            return usb_serial_close();

        case BT_VND_OP_GET_LPM_IDLE_TIMEOUT:
            //printf("BT_VND_OP_GET_LPM_IDLE_TIMEOUT\n");
            return 0;

        case BT_VND_OP_LPM_SET_MODE:
           // printf("BT_VND_OP_LPM_SET_MODE");
            sCallbacks->lpm_cb(BT_VND_OP_RESULT_SUCCESS);
            return 0;

        case BT_VND_OP_LPM_WAKE_SET_STATE:
            //printf("BT_VND_OP_LPM_WAKE_SET_STATE\n");
            return 0;
    }

    return -1;
}

static void cleanup()
{
    fprintf(stderr, "cleanup\n");
}

const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
    sizeof(bt_vendor_interface_t),
    init,
    op,
    cleanup
};

#ifndef _RFCOMM_H_
#define _RFCOMM_H_

#include "types.h"
#include "uniq.h"
#include "bt.h"
#include "sg.h"


#define RFC_FIRST_CHAN               1           /* first valid */
#define RFC_NUM_CHANS                30
#define RFC_CHNUM_REQ_ANY            0x80        /* allocate one for me */


#define PSM_RFCOMM                   0x0003


typedef uniq_t rfc_conn_t; /* a 0-valued connection ID is invalid and indicates error */

#define RFCCONNFMT    UNIQ_FMT"x"
#define RFCCONNCNV(x) UNIQ_CONV(x)


#define RFC_EVT_OPEN                 0 
#define RFC_EVT_RX                   1
#define RFC_EVT_MAY_SEND             2
#define RFC_EVT_CLOSE                3

#define RFC_TX_RET_ENTIRE_SENT       -1 /* entire SG was sent and is not freed. Do not use it */
#define RFC_TX_RET_STOP_SENDING      -2 /* this is temporary - wait for RFC_EVT_MAY_SEND */
#define RFC_TX_RET_NO_SUCH_CONN      -3 /* this is permanent - do not retry */
#define RFC_TX_RET_ERROR             -4 /* this is permanent - do not retry */


typedef void* (*rfcSvcAlloc)(void *userData, const struct bt_addr *peerAddr, uint8_t dlci);
typedef void (*rfcSvcEvent)(void *userData, void *instance, uint8_t evt, sg s);


bool rfcRegisterProfile(void);

uint8_t rfcListen(uint8_t chReq, rfcSvcAlloc allocF, rfcSvcEvent evtF, void *cbkData); /* returns DLCI. input = dlci specifically or RFC_CHNUM_REQ_ANY */
bool rfcStopListen(uint8_t ch); /* will not close any existing connections */
bool rfcConnect(const struct bt_addr *peerAddr, uint8_t ch, rfcSvcEvent evtF, void *cbkData, void *instance); /* true if you shoudl expect callback(s) */

int32_t rfcTx(rfc_conn_t conn, sg s); /* ret: non-negative values is bytes sent (and sheared off the front of the SG), negatives are other codes. if entire SG is sent, it is freed! */


#endif


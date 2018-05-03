/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef _SM_H_
#define _SM_H_

#include "export.h"
#include "hci.h"
#include "sg.h"
#include "types.h"

#define L2C_FIXED_CH_NUM_SM    6

#define SM_BLOCK_LEN      16
#define SM_SIG_LEN        12

/* PAIRING STATE AND STRUCTURE DEFINITION FOR HIGHER LAYER INTERFACE API */

/* STATE:        Initial state of pairing where nothing happens yet
 * TRANSIT FROM: SM_PAIR_STATE_PAIRED,SM_PAIR_STATE_CANCELED and SM_PAIR_STATE_FAILED
 * NOTE:         This is a termination state of smUnpair().
 */
#define SM_PAIR_STATE_NOT_PAIRED           0
/* STATE:        Pairing begins
 * TRANSIT FROM: SM_PAIR_STATE_NOT_PAIR, SM_PAIR_STATE_PAIRED
 * NOTE:         This is NOT a termination state of smPair(). Transited from SM_PAIR_STATE_PAIRED
 *               if security requirements are enhanced in the new pairing request.
 */
#define SM_PAIR_STATE_START                1
/* STATE:        Pairing finishes successfully
 * TRANSIT FROM: SM_PAIR_STATE_NOT_PAIRED or SM_PAIR_STATE_PAIRED
 * NOTE:         This is a termination state of smPair().
 */
#define SM_PAIR_STATE_PAIRED               2
/* STATE:        Pairing canceled
 * TRANSIT FROM: SM_PAIR_STATE_START
 * NOTE:         This is a termination state of smUnpair(). Reset to SM_PAIR_STATE_NOT_PAIRED w/o
 *               notification.
 */
#define SM_PAIR_STATE_CANCELED             3
/* STATE:        Pairing failed
 * TRANSIT FROM: SM_PAIR_STATE_START
 * NOTE:         This is a termination state of smPair(). Reset to SM_PAIR_STATE_NOT_PAIRED w/o
 *               notification.
 */
#define SM_PAIR_STATE_FAILED               4


/* ERROR:        No error
 * NOTE:         Come with SM_PAIR_STATE_PAIRED, SM_PAIR_STATE_START, SM_PAIR_STATE_CANCELED, and
 *               SM_PAIR_STATE_NOT_PAIRED.
 */
#define SM_PAIR_ERR_NONE                   0
/* ERROR:        Error where pairing was done with sufficient security requirements
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to fall back to SM_PAIR_PAIRED.
 */
#define SM_PAIR_ERR_ALREADY_PAIRED         1
/* ERROR:        Error where pairing is currently in progress. There is no guarantee on meeting
 *               the security requirements of the new pairing request when the pairing finishes.
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observers needs to fall back to either
 *               SM_PAIR_PAIRED or SM_PAIR_NOT_PAIRED depends on the previous state.
 */
#define SM_PAIR_ERR_IN_PROGRESS            2
/* ERROR:        Error where the parameters of pairing request are not provided or invalid
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observers needs to fall back to either
 *               SM_PAIR_PAIRED or SM_PAIR_NOT_PAIRED depends on the previous state.
 */
#define SM_PAIR_ERR_INVALID_PAIR_REQ       3
/* ERROR:        Error where the passkey mismatched during pairing
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_PASSKEY_FAILED         4
/* ERROR:        Error where the out of band pairing is not available locally
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_OOB_NOT_AVAILABLE      5
/* ERROR:        Error where the authentication requirements by an user cannot be met by either us
 *               or a peer device during pairing
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_AUTH_REQ_INFEASIBLE    6
/* ERROR:        Error where the confirmation value mismatched during pairing
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_CONF_VALUE_MISMATCHED  7
/* ERROR:        Error where pairing is not supported by either us or a peer device
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_PAIRING_NOT_SUPPORTED  8
/* ERROR:        Error where the encryption key length requested by a peer device is either invalid
 *               or insufficient
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_ENCR_KEY_SIZE          9
/* ERROR:        Error where multiple pairing attempts were made by either us or a peer device when
 *               a new pairing session was not yet allowed
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_REPEATED_ATTEMPT       10
/* ERROR:        Error where a peer device failed to provide the parsable/valid parameters in a SM
 *               command during pairing
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_INVALID_PARAM          11
/* ERROR:        Error where memory failed to be allocated for pairing
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_MEMORY                 12
/* ERROR:        Error where we failed to establish a L2CAP connection for SM protocol
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_L2C_CONN               13
/* ERROR:        Error where the device is not presented as a peer device for pairing/unpairing
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observers needs to fall back to either
 *               SM_PAIR_PAIRED or SM_PAIR_NOT_PAIRED depends on the previous state.
 */
#define SM_PAIR_ERR_NO_SUCH_DEVICE         14
/* ERROR:        Error where a peer device failed to provide an invalid/unexpected SM command
 *               during pairing
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_UNEXPECTED_SM_CMD      15
/* ERROR:        Error where we failed to send the SM command during pairing
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_SEND_SM_CMD            16
/* ERROR:        Error where we failed to encrypt a L2CAP connection with expected security
 *               requirement OR we failed/refused to respond to the unexpected encryption
 *               request/result
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_ENCR_CONN              17
/* ERROR:        Error where a peer device failed to provide an invalid/unexpected L2CAP event
 *               during pairing
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_UNEXPECTED_L2C_EVT     18
/* ERROR:        Error where either us or a peer device failed to continue the pairing procedure
 *               due to lack of response to any SM/L2CAP command/event, and the pairing stalled
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_STALLED                19
/* ERROR:        Error where there is unknown action taken by a peer device during OR pairing failed
 *               with an unspecified reason
 * NOTE:         Come with SM_PAIR_STATE_FAILED. Observer needs to transit to SM_PAIR_NOT_PAIRED.
 */
#define SM_PAIR_ERR_UNKNOWN                20

/* SM calls this to notify an observer about the pairing state change event.
 * observers need to cast pairStateChange to struct smPairStateChange, and there is no ownership
 * transition.
 */
typedef void (*smPairStateChangeCbk)(void *observerData, const void *pairStateChange,
                                     uniq_t observerId);

typedef uint8_t smPairState;
typedef uint8_t smPairErr;

struct smPairStateChange {
    smPairState pairState;
    smPairErr pairErr;
    struct bt_addr peerAddr;
};

struct smPairSecurityRequirements {
    bool bond; /* indicate whether the bonding should be performed with a peer device */
    bool mitm; /* indicate whether man-in-the-middle protection is required */
};


/* HELPER FUNCTIONS */

/* call this to perform CMAC calculation */
void smSignatureCalc(sg m, const uint8_t *key, uint8_t *sig) NEWBLUE_EXPORT;


/* ONE TIME SM INIT/DEINIT */

/* call this to perform on-time init of SM */
bool smInit(uint8_t ioCapability) NEWBLUE_EXPORT;
/* call this to perform on-time deinit of SM */
void smDeinit(void) NEWBLUE_EXPORT;


/* HIGHER LAYER INTERFACE API */

/* call this to register as an observer of the pairing state changes with peer devices */
uniq_t smRegisterPairObserver(void *observerData, smPairStateChangeCbk cbk) NEWBLUE_EXPORT;
/* call this to unregister as an observer of the pairing state changes with peer devices*/
void smUnregisterPairObserver(uniq_t observerId) NEWBLUE_EXPORT;
/* call this to pair with a remote LE device, the state of pairing will notify via
 * smPairStateChangeCbk
 */
void smPair(const struct bt_addr *addr,
    const struct smPairSecurityRequirements *secReqs) NEWBLUE_EXPORT;
/* call this to cancel/unpair with a remote LE device, the state of pairing will notify via
 * smPairStateChangeCbk
 */
void smUnpair(const struct bt_addr *addr) NEWBLUE_EXPORT;
/* call this to generate a resolvable private address for advertising */
bool smGenResolvableAddr(struct bt_addr *addr);
/* call this to generate a non-resolvable private address for advertising */
bool smGenNonResolvableAddr(struct bt_addr *addr);


/* EXPOSED ONLY FOR UNITTEST PURPOSE */

void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key);
void smCalcConfVal(uint8_t* out, const uint8_t *k, const uint8_t *r, uint64_t pres, uint64_t preq,
                   bool iat, uint64_t ia, bool rat, uint64_t ra);
void smCalcKey(uint8_t *out, const uint8_t *k, const uint8_t *r1, const uint8_t *r2);
uint32_t smAddressHash(uint32_t r, const uint8_t *key);

#endif /* _SM_H_ */

#ifndef _L2CAP_H_
#define _L2CAP_H_

#include "multiNotif.h"
#include "types.h"
#include "uniq.h"
#include "hci.h"
#include "bt.h"
#include "sg.h"


#define OUR_MTU  0xEFFF /* no reason to limit it pointlessly */

typedef uint16_t psm_t;
typedef uniq_t l2c_handle_t; /* a 0-valued handle is invalid and indicates error */
typedef uniq_t acl_handle_t; /* a 0-valued handle is invalid and indicates error */

#define HANDLEFMT    UNIQ_FMT"x"
#define HANDLECNV(x) UNIQ_CONV(x)


/* ONE_TIME L2CAP INIT */

/* call this to perform one-time init of l2cap */
int l2cInit(void);
void l2cDeinit(void);


#define L2C_TX_ACCEPTED      0 /* data will be sent if connection remains - done */
#define L2C_TX_NO_CONN       1 /* no such connection - do not retry */
#define L2C_TX_TRY_LATER     2 /* no send capacity now - retry later */
#define L2C_TX_ERROR         3 /* some other error - do not retry */


/* HIGHER LAYER INTERFACE API */


/* callbacks for connections */
struct l2cEncrState {
    bool isEncr;
    bool isMitmSafe;
};

#define L2C_STATE_OPEN       0 /* dataPtr points to l2c_handle_t */
#define L2C_STATE_MTU        1 /* dataPtr points to a uint16_t mtu */
#define L2C_STATE_ENCR       2 /* dataPtr points l2cEncrState */
#define L2C_STATE_RX         3 /* dataPtr points to an sg */
#define L2C_STATE_CLOSED     4 /* dataPtr points nowhere, or error on openining */ 
typedef void (*l2cStateCbk)(void *userData, void *instance, uint8_t state, const void *data, uint32_t len);

#define SVC_ALLOC_SUCCESS             0 /* must alloc state, success */
#define SVC_ALLOC_DEMAND_ENCR         1 /* return this instead of calling l2cApiDemandEncryption(...,false). do not allocate - you will be called again */
#define SVC_ALLOC_DEMAND_AUTH         2 /* return this instead of calling l2cApiDemandEncryption(..., true). do not allocate - you will be called again */
#define SVC_ALLOC_FAIL_SECURITY       3 /* not secure enough connection */
#define SVC_ALLOC_FAIL_BEING_REMOVED  4 /* being deallocated */
#define SVC_ALLOC_FAIL_NO_CONNS       5 /* do not support conns */
#define SVC_ALLOC_WAIT_FOR_AUTHORIZ   6 /* must alloc state but need user's OK. Standby */
#define SVC_ALLOC_FAIL_OTHER          7 /* some other failure */

/* a PSM-based service is described thusly */
struct l2cServicePsmDescriptor {

    void *userData; /* passed to all callbacks */

    /* these are null if connectionless only */
    uint8_t (*serviceInstanceAlloc)(void *userData, l2c_handle_t handle, void **instanceP); /* return SVC_ALLOC_* */
    l2cStateCbk serviceInstanceStateCbk; /* free when called with the close state */
    uint16_t mtu; /* max data size we're willing to accept */

    /* is null if connection-oriented only */
    void (*serviceConnectionlessRx)(void *userData, l2c_handle_t handle, sg payload);
};


/* a FixedCh service is described thusly */
struct l2cServiceFixedChDescriptor {

    void *userData; /* passed to all callbacks */

    /* called when ACL connection with a party supporting this channel is opened */
    uint8_t (*serviceFixedChAlloc)(void *userData, l2c_handle_t handle, void **instanceP); /* return SVC_ALLOC_* */

    /* called when data is received */
    l2cStateCbk serviceFixedChStateCbk; /* free when called with the close state */
};


/* call this to register a PSM-based service. pass 0 for any psm. returns zero or your psm */
psm_t l2cApiServicePsmRegister(psm_t psm, const struct l2cServicePsmDescriptor *service);

/* call this to unregister a PSM-based service. callback called when done, if provided */
bool l2cApiServicePsmUnregister(psm_t psm, void (*doneCbk)(void*), void *cbkData);


/* call this to register a FixedCh service. */
bool l2cApiServiceFixedChRegister(uint16_t chan, const struct l2cServiceFixedChDescriptor *service);

/* call this to unregister a FixedCh service. callback called when done, if provided */
bool l2cApiServiceFixedChUnregister(uint16_t chan, void (*doneCbk)(void*), void *cbkData);


/* call this to connect to a remote PSM-based service Expect an MTU callback. */
bool l2cApiCreatePsmConnection(psm_t psm, const struct bt_addr *addr, uint16_t mtu, l2cStateCbk stateCbk, void *userData, void *instance);

/* call this to connect to a remote FixedCh service. Do not expect an MTU callback. */
bool l2cApiCreateFixedChConnection(uint16_t chan, const struct bt_addr *addr, l2cStateCbk stateCbk, void *userData, void *instance);


/* call this to request a connection parameter update for a given connection */
typedef void (*l2cConnUpdateDoneCbk)(void *userData, bool success);
bool l2cApiUpdateConnectionParams(l2c_handle_t handle, uint16_t intMin, uint16_t intMax, uint16_t latency, uint16_t timeout, l2cConnUpdateDoneCbk doneCbk, void *userData);

/* external code can use this to get remote's bt addr */
bool l2cApiGetBtAddr(l2c_handle_t handle, struct bt_addr *addrP);

/* external code can use this to get L2CAP's ACL handle */
acl_handle_t l2cApiGetAclHandle(l2c_handle_t handle);

/* external code can use this to see if a connection is encrypted */
bool l2cApiIsConnEncrypted(l2c_handle_t handle);

/* external code can use this to see if a connection is encrypted using a mitm-protected ket */
bool l2cApiIsConnMitmSafe(l2c_handle_t handle);

/* external code can use this to demand encrypted be used */
bool l2cApiDemandEncryption(l2c_handle_t handle, bool demandMitmSafe);

/* external code can use this to notify other size of athorization (only if your svc alloc returned SVC_ALLOC_WAIT_FOR_AUTHORIZ). */
bool l2cApiUserAthorizNotify(l2c_handle_t handle, bool authorized);

/* external code can use this to disconnect a channel */
void l2cApiDisconnect(l2c_handle_t handle);

/* external code can use this to send to the other side - never blocks. returns L2C_TX_* */
uint8_t l2cApiDataTx(l2c_handle_t handle, sg data);

/* exteranl code can use this to know when it shoudl retry its write. Do not do that in the callback! - queue it for later */
uniq_t l2cApiRegisterWriteBufNotif(multiNotifCbk cbk, void *cbkData);

/* exteranl code can use this to unregister from notif created by l2cApiRegisterWriteBufNotif() */
void l2cApiUnegisterWriteBufNotif(uniq_t notifHandle);

/* if no connection exists, one will *not* be created. if sending does not work, there is no retry callback */
uint8_t l2cApiConnectionlessDataTx(const struct bt_addr *addr, psm_t psm, sg data);



/* LOWER LAYER INTERFACE API */

/* lower layer calls this when new ACL link is up */
void l2cAclLinkUp(hci_conn_t aclConn, const struct bt_addr *addr, bool isMaster, bool isEncrypted, bool isMitmSafe);
/* lower layer calls this when an ACL link is down */
void l2cAclLinkDown(hci_conn_t aclConn);
/* lower layer calls this when new ACL data arrives */
void l2cAclDataRx(hci_conn_t aclConn, sg packet, bool first);
/* lower layer calls this to tell us that a data credit just became available. called directly. DO NOT BLOCK! */
void l2cAclCreditAvail(bool le);
/* lower layer calls this when connection parameters change */
void l2cAclLinkParamsChange(hci_conn_t aclConn, bool success, uint16_t interval, uint16_t latency, uint16_t timeout);
/* lower layer calls this when connection encryption changes. called with FALSE if encr failed since BT cannot realyl go from encrypted to not */
void l2cAclLinkEncrChange(hci_conn_t aclConn, bool isEncrypted, bool isMitmSafe);


#endif


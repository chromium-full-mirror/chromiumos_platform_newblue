#ifndef _L2CAP_H_
#define _L2CAP_H_

#include "types.h"
#include "uniq.h"
#include "bt.h"


#define OUR_MTU  0xEFFF /* no reason to limit it pointlessly */

typedef uint16_t psm_t;
typedef uniq_t l2c_handle_t; /* a 0-valued handle is invalid and indicates error */
#define HANDLEFMT "0x%016llx"
#define HANDLECNV (unsigned long long)


/* ONE_TIME L2CAP INIT */

/* call this to perform one-time init of l2cap */
int l2cInit(void);




/* HIGHER LAYER INTERFACE API */


/* callbacks for connections */
#define L2C_STATE_OPEN       0 /* dataPtr points to l2c_handle_t */
#define L2C_STATE_MTU        1 /* dataPtr points to a uint32_t mtu */
#define L2C_STATE_RX         2 /* dataPtr points to data, len to length */
#define L2C_STATE_CLOSED     3 /* or error on openining */ 
typedef void (*l2cStateCbk)(void *userData, void *instance, uint8_t state, const void *data, uint32_t len);

/* a PSM-based service is described thusly */
struct l2cServicePsmDescriptor {

    void *userData; /* passed to all callbacks */

    /* these are null if connectionless only */
    void* (*serviceInstanceAlloc)(void *userData, l2c_handle_t handle);
    l2cStateCbk serviceInstanceStateCbk; /* free when called with the close state */

    /* is null if connection-oriented only */
    void (*serviceConnectionlessRx)(void *userData, l2c_handle_t handle, const void *data, uint32_t size);
};


/* a FixedCh service is described thusly */
struct l2cServiceFixedChDescriptor {

    void *userData; /* passed to all callbacks */

    /* called when ACL connection with a party supporting this channel is opened */
    void* (*serviceFixedChAlloc)(void *userData, l2c_handle_t handle);

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
bool l2cApiCreatePsmConnection(psm_t psm, const struct bt_addr *addr, l2cStateCbk stateCbk, void *userData, void *instance);

/* call this to connect to a remote FixedCh service. Do not expect an MTU callback. */
bool l2cApiCreateFixedChConnection(uint16_t chan, const struct bt_addr *addr, l2cStateCbk stateCbk, void *userData, void *instance);


/* call this to request a connection parameter update for a given connection */
typedef void (*l2cConnUpdateDoneCbk)(void *userData, bool success);
bool l2cApiUpdateConnectionParams(l2c_handle_t handle, uint16_t intMin, uint16_t intMax, uint16_t latency, uint16_t timeout, l2cConnUpdateDoneCbk doneCbk, void *userData);

/* external code can use this to get remote's bt addr */
bool l2cApiGetBtAddr(l2c_handle_t handle, struct bt_addr *addrP);

/* external code can use this to disconnect a channel */
void l2cApiDisconnect(l2c_handle_t handle);

/* external code can use this to send to the other side */
bool l2cApiDataTx(l2c_handle_t handle, const void *data, uint32_t size);

/* if no connection exists, one will *not* be created */
bool l2cApiConnectionlessDataTx(const struct bt_addr *addr, psm_t psm, const void *data, uint32_t size);


/* LOWER LAYER INTERFACE API */

/* lower layer calls this when new ACL link is up */
void l2cAclLinkUp(uint16_t aclConn, const struct bt_addr *addr, bool isMaster);
/* lower layer calls this when an ACL link is down */
void l2cAclLinkDown(uint16_t aclConn);
/* lower layer calls this when new ACL data arrives */
void l2cAclDataRx(uint16_t aclConn, const void *data, uint32_t len, bool first);
/* this layer calls this to send ACL data */
void l2cAclDataTx(uint16_t aclConn, const void *data, uint32_t len);
/* this layer calls this to establish a link. *addr need not stay valid after call */
void l2cAclLinkEstablish(const struct bt_addr *addr);
/* this layer calls this to close a link, or cancel one being opened */
void l2cAclLinkClose(const struct bt_addr *addr);
/* this layer calls this to udpate a connection's LE parameters */
void l2cAclUpdateParams(uint16_t aclConn, uint16_t minInt, uint16_t maxInt, uint16_t lat, uint16_t to);
/* lower layer calls this when connection parameters change */
void l2cAclLinkParamsChange(uint16_t aclConn, bool success, uint16_t interval, uint16_t latency, uint16_t timeout);

#endif


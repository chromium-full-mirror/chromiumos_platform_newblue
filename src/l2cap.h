#ifndef _L2CAP_H_
#define _L2CAP_H_

#include "types.h"
#include "bt.h"


typedef uint16_t psm_t;
typedef uint64_t l2c_handle_t; /* a 0-valued handle is invalid and indicates error */
#define HANDLEFMT "0x%016llx"
#define HANDLECNV (long long unsigned)


/* ONE_TIME L2CAP INIT */

/* call this to perform one-time init of l2cap */
int l2cInit(void);




/* HIGHER LAYER INTERFACE API */

/* a PSM-based service is described thusly */
struct l2cServicePsmDescriptor {

    void *userData; /* passed to all callbacks */

    /* all these are null if connectionless only */
    void* (*serviceInstanceAlloc)(void* userData, l2c_handle_t handle);
    void (*serviceInstanceFree)(void* userData, void* instance);
    void (*serviceInstanceRx)(void* userData, void* instance, const void *data, uint32_t size);

    /* isnull if connection-oriented only */
    void (*serviceConnectionlessRx)(void* userData, l2c_handle_t handle, const void *data, uint32_t size);
};


/* a FixedCh service is described thusly */
struct l2cServiceFixedChDescriptor {

    void *userData; /* passed to all callbacks */

    /* called when ACL connection with a party supporting this channel is opened */
    void* (*serviceFixedChAlloc)(void* userData, l2c_handle_t handle);

    /* called when data is received */
    void (*serviceFixedChRx)(void* userData, void* instance, const void *data, uint32_t size);

    /* called when the underlying connection is closed */
    void (*serviceFixedChFree)(void* userData, void* instance);
};


/* call this to register a PSM-based service. pass 0 for any psm. returns zero or your psm */
psm_t l2cApiServicePsmRegister(psm_t psm, const struct l2cServicePsmDescriptor *service);

/* call this to unregister a PSM-based service. callback called when done, if provided */
bool l2cApiServicePsmUnregister(psm_t psm, void (*doneCbk)(void*), void* cbkData);


/* call this to register a FixedCh service. */
bool l2cApiServiceFixedChRegister(uint16_t chan, const struct l2cServiceFixedChDescriptor *service);

/* call this to unregister a FixedCh service. callback called when done, if provided */
bool l2cApiServiceFixedChUnregister(uint16_t chan, void (*doneCbk)(void*), void* cbkData);

/* callbacks for connections */
#define L2C_STATE_OPEN       0 /* dataPtr points to l2c_handle_t, len has mtu */
#define L2C_STATE_RX         1 /* dataPtr points to data, len to length */
#define L2C_STATE_CLOSED     2 /* or error on openining */ 
typedef void (*l2cStateCbk)(void *cbkData, uint8_t state, const void *data, uint32_t len);

/* call this to connect to a remote PSM-based service */
bool l2cApiCreatePsmConnection(psm_t psm, const struct bt_addr *addr, l2cStateCbk stateCbk, void *cbkData);

/* call this to connect to a remote FixedCh service */
bool l2cApiCreateFixedChConnection(uint16_t chan, const struct bt_addr *addr, l2cStateCbk stateCbk, void *cbkData);





/* SERVICE INTERFACE API */

/* service code can use this to get remote's bt addr */
bool l2cSvcGetBtAddr(l2c_handle_t handle, struct bt_addr* addrP);

/* service code can use this to disconnect a channel */
void l2cSvcDisconnect(l2c_handle_t handle);

/* service code can use this to send to the other side */
bool l2cSvcDataTx(l2c_handle_t handle, const void* data, uint32_t size);



/* LOWER LAYER INTERFACE API */

/* lower layer calls this when new ACL link is up */
void l2cAclLinkUp(uint16_t aclConn, const struct bt_addr* addr);
/* lower layer calls this when an ACL link is down */
void l2cAclLinkDown(uint16_t aclConn);
/* lower layer calls this when new ACL data arrives */
void l2cAclDataRx(uint16_t aclConn, const void *data, uint32_t len);
/* this layer calls this to send ACL data */
void l2cAclDataTx(uint16_t aclConn, const void *data, uint32_t len);
/* this layer calls this to establish a link */
void l2cAclLinkEstablish(const struct bt_addr* addr);
/* this layer calls this to close a link, or cancel one being opened */
void l2cAclLinkClose(const struct bt_addr* addr);

#endif


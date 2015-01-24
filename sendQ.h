#ifndef _SEND_Q_H_
#define _SEND_Q_H_

#include "types.h"
#include "l2cap.h"
#include "sg.h"

struct sendQ;

/*
 * Due to how L2C does flow control, keeping track of packets to send if a pain.
 * This structure aims to help you in that respect by aiding in queueing packets,
 * limiting the length of said queue, and allowing removing queue packets for
 * dead connections.
 */


struct sendQ* sendQueueAlloc(uint32_t maxMsgsPerConn);
void sendQueueFree(struct sendQ *q);

bool sendQueueCanTx(struct sendQ *q, l2c_handle_t conn); /* the answer is NOT authoritative, but may be a good hint */
bool sendQueueTx(struct sendQ *q, l2c_handle_t conn, sg s); /* send unless error or maxMsgs limit reached */
bool sendQueueForceTx(struct sendQ *q, l2c_handle_t conn, sg s); /* send unless eror */
void sendQueueDelPackets(struct sendQ *q, l2c_handle_t conn);




#endif


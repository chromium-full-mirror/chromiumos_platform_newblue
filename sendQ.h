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


struct sendQ* sendQueueAlloc(uint32_t maxMsgsPerConn, const char* name);
void sendQueueFree(struct sendQ *q); //delete is async

bool sendQueueTx(struct sendQ *q, l2c_handle_t conn, sg s);
void sendQueueDelPackets(struct sendQ *q, l2c_handle_t conn);




#endif


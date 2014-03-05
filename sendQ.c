#include "l2cap.h"
#include "sendQ.h"
#include "log.h"
#include "mt.h"

struct packet {
     struct packet *next;
     l2c_handle_t   conn;
     sg             s;
};

struct sendQ {
    pthread_mutex_t lock;
    uniq_t          l2cNotifH;

    struct packet  *head;
    struct packet  *tail;
    uint32_t        maxMsgsPerConn;
};



/*
 * FUNCTION: sendQueueTryTx
 * USE:      Try to send for ther queue
 * PARAMS:   the q - the SendQ object
 * RETURN:   NONE
 * NOTES:    call with q->lock taken
 */
static void sendQueueTryTx(struct sendQ *q)
{
    bool trySend = true;
    struct packet *t;
    uint8_t ret;

    while (q->head) {

        /* try a send */
        ret = l2cApiDataTx(q->head->conn, q->head->s);
        switch(ret) {
        case L2C_TX_ACCEPTED:
            /* nothing to do here */
            break;
        case L2C_TX_ERROR:
            logw("L2C TX fail on "HANDLEFMT"\n", HANDLECNV(q->head->conn));
        case L2C_TX_NO_CONN:
            sgFree(q->head->s);
            break;
        case L2C_TX_TRY_LATER:
            return;
        }

        if (q->tail == q->head) {
            free(q->head);
            q->tail = NULL;
            q->head = NULL;
        } else {
            t = q->head;
            q->head = q->head->next;
            free(t);
        }
    }
}

/*
 * FUNCTION: sendQueueL2cBufNotif
 * USE:      Called by L2C when a TX buffer potentially opens up
 * PARAMS:   cbkData - for us this is the sendQ
 *           notifData - unused for L2C send notifications
 *           handle - handle of our notification handler. unused.
 * RETURN:   NONE
 * NOTES:
 */
static void sendQueueL2cBufNotif(void *cbkData, const void *notifData, uniq_t handle)
{
    struct sendQ *q = (struct sendQ*)cbkData;

    if (pthread_mutex_trylock(&q->lock))
        return;
    sendQueueTryTx(q);
    pthread_mutex_unlock(&q->lock);
}

/*
 * FUNCTION: sendQueueAlloc
 * USE:      Create a sendQ
 * PARAMS:   maxMsgsPerConn - max messages to allow per connection
 * RETURN:   the SendQ object or NULL on error
 * NOTES:
 */
struct sendQ* sendQueueAlloc(uint32_t maxMsgsPerConn)
{
    struct sendQ *q = (struct sendQ*)calloc(1, sizeof(struct sendQ));

    if (!q)
        goto fail_oom;

    q->maxMsgsPerConn = maxMsgsPerConn;

    if (pthread_mutex_init(&q->lock, NULL))
        goto fail_mutex;

    q->l2cNotifH = l2cApiRegisterWriteBufNotif(sendQueueL2cBufNotif, q);
    if (!q->l2cNotifH)
        goto fail_notif_reg;

    return q;


fail_notif_reg:
    pthread_mutex_destroy(&q->lock);

fail_mutex:
    free(q);

fail_oom:
    return NULL;
}

/*
 * FUNCTION: sendQueueFree
 * USE:      Free a sendQ
 * PARAMS:   q - the SendQ
 * RETURN:   NONE
 * NOTES:    The free itself is async and will complete at a later time
 */
void sendQueueFree(struct sendQ *q)
{
    struct packet *p;

    pthread_mutex_lock(&q->lock);
    while (q->head) {
        p = q->head;
        q->head = q->head->next;
        sgFree(p->s);
        free(p);
    }
    pthread_mutex_destroy(&q->lock);
    free(q);
}

/*
 * FUNCTION: sendQueueTxInt
 * USE:      Enqueue a packet to send using the sendQ, enforce quotas if needed
 * PARAMS:   q - the SendQ
 *           conn - the L2C connection
 *           s - the SG to send
 *           enforceQuotas - true to enforce quotas, false else
 * RETURN:   false on error
 * NOTES:
 */
static bool sendQueueTxInt(struct sendQ *q, l2c_handle_t conn, sg s, bool enforceQuotas)
{
    uint32_t msgsForConnAlready = 0;
    struct packet *t;
    bool ret = false;

    pthread_mutex_lock(&q->lock);

    for (t = q->head; t; t = t->next)
        if (t->conn == conn)
            msgsForConnAlready++;

    if (!enforceQuotas || msgsForConnAlready < q->maxMsgsPerConn) {
        t = (struct packet*)calloc(1, sizeof(struct packet));
        if (t) {

            t->conn = conn;
            t->s = s;
            if (q->tail)
                q->tail->next = t;
            else
                q->head = t;
            q->tail = t;

            ret = true;
        }
    }

    sendQueueTryTx(q);
    pthread_mutex_unlock(&q->lock);
    return ret;
}

/*
 * FUNCTION: sendQueueForceTx
 * USE:      Enqueue a packet to send using the sendQ, ignore quotas
 * PARAMS:   q - the SendQ
 *           conn - the L2C connection
 *           s - the SG to send
 * RETURN:   false on error
 * NOTES:
 */
bool sendQueueForceTx(struct sendQ *q, l2c_handle_t conn, sg s)
{
    return sendQueueTxInt(q, conn, s, false);
}

/*
 * FUNCTION: sendQueueTx
 * USE:      Enqueue a packet to send using the sendQ
 * PARAMS:   q - the SendQ
 *           conn - the L2C connection
 *           s - the SG to send
 * RETURN:   false on error of over limit
 * NOTES:
 */
bool sendQueueTx(struct sendQ *q, l2c_handle_t conn, sg s)
{
    return sendQueueTxInt(q, conn, s, true);
}

/*
 * FUNCTION: sendQueueCanTx
 * USE:      Says if a packet may be queneued at this point
 * PARAMS:   q - the SendQ
 *           conn - the L2C connection
 * RETURN:   true if so, false else
 * NOTES:    Note that the reply is stale even by the time it is received and should be use just as a hint
 */
bool sendQueueCanTx(struct sendQ *q, l2c_handle_t conn)
{
    uint32_t msgsForConnAlready = 0;
    struct packet *t;
    bool ret;

    pthread_mutex_lock(&q->lock);

    for (t = q->head; t; t = t->next)
        if (t->conn == conn)
            msgsForConnAlready++;

    ret = msgsForConnAlready < q->maxMsgsPerConn;

    sendQueueTryTx(q);
    pthread_mutex_unlock(&q->lock);

    return ret;
}

/*
 * FUNCTION: sendQueueDelPackets
 * USE:      Delete all queued packets to a given connection
 * PARAMS:   q - the SendQ
 *           conn - the L2C connection whose packets we want to kill
 * RETURN:   NONE
 * NOTES:
 */
void sendQueueDelPackets(struct sendQ *q, l2c_handle_t conn)
{
    struct packet *t, *p = NULL;

    pthread_mutex_lock(&q->lock);

    t = q->head;
    while (t) {
        if (t->conn == conn) {

            sgFree(t->s);

            if (q->tail == t)
                q->tail = p;

            if (p) {
                p->next = t->next;
                free(t);
                t = p->next;
            } else {
                q->head = t->next;
                free(t);
                t = q->head;
            }

        } else {
            p = t;
            t = t->next;
        }
    }

    sendQueueTryTx(q);
    pthread_mutex_unlock(&q->lock);
}


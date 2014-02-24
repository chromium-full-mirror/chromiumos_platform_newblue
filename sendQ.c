#include <pthread.h>
#include "l2cap.h"
#include "sendQ.h"
#include "log.h"

struct packet {
     struct packet *next;
     l2c_handle_t   conn;
     sg             s;
};

struct sendQ {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_t       worker;
    uniq_t          l2cNotifH;

    struct packet  *head;
    struct packet  *tail;
    uint32_t        maxMsgsPerConn;
    bool            pleaseDie;
    char            name[];
};



/*
 * FUNCTION: sendQueueWorker
 * USE:      The worker thread for the SendQ
 * PARAMS:   the Q - the SendQ object
 * RETURN:   always NULL
 * NOTES:    Runs until stopped. On request to stop cleans up.
 */
static void* sendQueueWorker(void *theQ)
{
    struct sendQ *q = (struct sendQ*)theQ;
    bool trySend = true;
    struct packet *t;
    uint8_t ret;

    pthread_mutex_lock(&q->lock);

    /* some init */
    pthread_setname_np(pthread_self(), q->name);

    /* our loop */
    while (1) {
        bool freeHead = true;

        /* die if needed */
        if (q->pleaseDie)
            break;

        /* wait, if needed */
        if (!trySend)
            pthread_cond_wait(&q->cond, &q->lock);
        trySend = true;

        if (!q->head) {
            trySend = false;
            continue;
        }

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
        case  L2C_TX_TRY_LATER:
            freeHead = false;
            trySend = false; /* wait next time */
            break; 
        }

        if (freeHead) {
            if (q->tail == q->head) {
                free(q->head);
                q->tail = NULL;
                q->head = NULL;
                trySend = false;
            } else {
                t = q->head;
                q->head = q->head->next;
                free(t);
            }
        }
    }

    /* some cleanup */
    l2cApiUnegisterWriteBufNotif(q->l2cNotifH);
    while (q->head) {
        t = q->head;
        q->head = q->head->next;
        sgFree(t->s);
        free(t);
    }
    pthread_cond_destroy(&q->cond);
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);

    return NULL;
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
    pthread_cond_signal(&q->cond);
}

/*
 * FUNCTION: sendQueueAlloc
 * USE:      Create a sendQ
 * PARAMS:   maxMsgsPerConn - max messages to allow per connection
 *           name - the name (for debugging & thread naming)
 * RETURN:   the SendQ object or NULL on error
 * NOTES:
 */
struct sendQ* sendQueueAlloc(uint32_t maxMsgsPerConn, const char* name)
{
    struct sendQ *q = (struct sendQ*)calloc(1, sizeof(struct sendQ) + strlen(name) + 1);
    char *nameCopy;

    if (!q)
        goto fail_oom;

    strcpy(q->name, name);
    q->maxMsgsPerConn = maxMsgsPerConn;

    if (pthread_mutex_init(&q->lock, NULL))
        goto fail_mutex;

    if (pthread_cond_init(&q->cond, NULL))
        goto fail_cond;

    q->l2cNotifH = l2cApiRegisterWriteBufNotif(sendQueueL2cBufNotif, q);
    if (!q->l2cNotifH)
        goto fail_notif_reg;

    if (pthread_create(&q->worker, NULL, sendQueueWorker, q))
        goto fail_thread;

    pthread_detach(q->worker);

    return q;


fail_thread:
   l2cApiUnegisterWriteBufNotif(q->l2cNotifH);

fail_notif_reg:
    pthread_cond_destroy(&q->cond);

fail_cond:
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
    pthread_mutex_lock(&q->lock);
    q->pleaseDie = true;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

/*
 * FUNCTION: sendQueueTx
 * USE:      Enqueue a packet to send using the sendQ
 * PARAMS:   q - the SendQ
 *           conn - the L2C connection
 *           s - the SG to send
 * RETURN:   false on error
 * NOTES:
 */
bool sendQueueTx(struct sendQ *q, l2c_handle_t conn, sg s)
{
    uint32_t msgsForConnAlready = 0;
    struct packet *t;
    bool ret = false;

    pthread_mutex_lock(&q->lock);

    for (t = q->head; t; t = t->next)
        if (t->conn == conn)
            msgsForConnAlready++;

    if (msgsForConnAlready < q->maxMsgsPerConn) {
        t = (struct packet*)calloc(1, sizeof(struct packet));
        if (t) {

            t->conn = conn;
            t->s = s;
            if (q->tail)
                q->tail->next = t;
            else
                q->head = t;
            q->tail = t;
        }
    }

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

    pthread_mutex_unlock(&q->lock);
}


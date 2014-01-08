#include <pthread.h>
#include <stdlib.h>
#include "workQueue.h"

struct workQueue {
    int posR, posW, size;
    int status;

    pthread_mutex_t lock;
    pthread_cond_t cond;

    void *buf[];
};

/*
 * FUNCTION: workQueueAlloc
 * USE:      Create and return a work queue
 * PARAMS:   numSlots - how many slots the queue should have
 * RETURN:   The workqueue or NULL on error
 * NOTES:
 */
struct workQueue* workQueueAlloc(int numSlots)
{
    struct workQueue* q;

    numSlots++; /* one always unused */
    q = malloc(sizeof(struct workQueue*) + sizeof(void* [numSlots]));

    if (q) {

        q->posR = 0;
        q->posW = 0;
        q->status = 0;
        q->size = numSlots;

        if (pthread_mutex_init(&q->lock, NULL)) {
            free(q);
            q = NULL;
        }
        else if (pthread_cond_init(&q->cond, NULL)) {
            pthread_mutex_destroy(&q->lock);
            free(q);
            q = NULL;
        }
    }
    return q;
}

/*
 * FUNCTION: workQueueFree
 * USE:      Free a workqueue
 * PARAMS:   workQueue - the workQueue
 *           freeCbk - callback to call on each existing request
 * RETURN:   NONE
 * NOTES:    Your job to make sure nobody's waiting on this queue when you do this
 */
void workQueueFree(struct workQueue* q, void (*freeCbk)(void*))
{
    pthread_mutex_lock(&q->lock);
    while (q->posR != q->posW) {

        if(freeCbk)
            freeCbk(q->buf[q->posR]);

        if (++q->posR == q->size)
            q->posR = 0;
    }
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
    free(q);
}

/*
 * FUNCTION: workQueueGet
 * USE:      Get some work from the queue
 * PARAMS:   workQueue - the workQueue
 *           workP - work pointer will be written here
 * RETURN:   Current status (0 for none, else whatever
 *           workQueueWakeAll has set it to)
 * NOTES:    Will block untill work is available or woken explicitly
 */
int workQueueGet(struct workQueue* q, void** workP)
{
    int ret = 0;

    pthread_mutex_lock(&q->lock);
    do {
        pthread_cond_wait(&q->cond, &q->lock);
    } while (q->posR == q->posW && !q->status);

    if (q->status)
        ret = q->status;
    else {
        *workP = q->buf[q->posR++];
        if (q->posR == q->size)
            q->posR = 0;
    }
    pthread_mutex_unlock(&q->lock);

    return ret;
}

/*
 * FUNCTION: workQueuePut
 * USE:      Put some work into the queue
 * PARAMS:   workQueue - the workQueue
 *           work - the work to enqueue
 * RETURN:   true if all OK, false if queue is out of space
 * NOTES:
 */
bool workQueuePut(struct workQueue *q, void* work)
{
    int newIdx;
    bool ret = true;

    pthread_mutex_lock(&q->lock);
    newIdx = q->posW + 1;
    if (newIdx == q->size)
        newIdx = 0;

    if (newIdx == q->posR) //list is full
        ret = false;
    else {
        q->buf[q->posW] = work;
        q->posW = newIdx;
    }
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);

    return ret;
}

/*
 * FUNCTION: workQueueWakeAll
 * USE:      Wake all workers blocked on the queue
 * PARAMS:   workQueue - the workQueue
 *           status - the status to broadcast, NONZERO ONLY!
 *           to restore the queue to working state, call with zero
 * RETURN:   NONE
 * NOTES:    Workers will be woken. It is up to you to make sure they know why
 */
void workQueueWakeAll(struct workQueue* q, int status)
{
    pthread_mutex_lock(&q->lock);
    q->status = status;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}






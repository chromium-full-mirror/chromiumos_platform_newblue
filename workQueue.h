#ifndef _WORK_QUEUE_H_
#define _WORK_QUEUE_H_

#include "types.h"

struct workQueue;

//Create and return a work queue
struct workQueue* workQueueAlloc(int numSlots);

//Free a workqueue (be sure nobody's using it), has callback for remaining requests
void workQueueFree(struct workQueue* q, void (*freeCbk)(void*));

//Get some work. Blocks. Returns status
int workQueueGet(struct workQueue* q, void** workP);

//Enqueue some work. Returns false if there ar eno free slots in the queue
bool workQueuePut(struct workQueue *q, void* work);

//Wake all workers waiting on the queue. What they do then is up to you.
void workQueueWakeAll(struct workQueue* q, int status);


#endif


#ifndef _MULTI_NOTIF_H_
#define _MULTI_NOTIF_H_

#include "types.h"
#include "uniq.h"


typedef void (*multiNotifCbk)(void *cbkData, const void *notifData, uniq_t handle);

struct multiNotifList;

/* api for notifiers */
struct multiNotifList *multiNotifCreate(void);
void multiNotifDestroy(struct multiNotifList *nl);
void multiNotifNotify(struct multiNotifList *nl, const void *notifData);

/* api for notifiees */
uniq_t multiNotifRegister(struct multiNotifList *nl, multiNotifCbk cbk, void *cbkData);
void multiNotifUnregister(struct multiNotifList *nl, uniq_t notifRegisterHandle);





#endif



#ifndef _CONFIG_H_
#define _CONFIG_H_



/*
 * L2CAP layer will allow at most this many concurrent outstanding work requests
 * to the services running on top of it / under it. If you have a lot of
 * sometimes-slow services, you may want to set this high. All requests that
 * overflow this limit will be dropped.
 */
#define L2C_SVC_NUM_OUTSTANDING_REQS    512

/*
 * How long to wait before closing a link (in case anyone else wants it
 * value is in msec
 */
#define L2C_ACL_LINK_TIMOUT             5000


#endif


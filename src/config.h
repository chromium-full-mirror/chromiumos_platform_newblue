#ifndef _CONFIG_H_
#define _CONFIG_H_



/*
 * L2CAP layer will allow at most this many concurrent outstanding work requests
 * to the services running on top of it. If you have a lot of sometimes-slow
 * services, you may want to set this high. All requests that overflow this limit
 * will be dropped.
 */
#define L2C_SVC_NUM_OUTSTANDING_REQS    128

#endif


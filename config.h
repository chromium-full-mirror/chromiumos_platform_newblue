#ifndef _CONFIG_H_
#define _CONFIG_H_



/*
 * AAPI will try to queue this much per socket before shutting off flow
 * Due to latencies in shutting off slow, the real amount queued could be
 * more.
 */
#define PREF_PATH                           "/data/data/com.android.bluetooth/bt.newblue.prefs"

/*
 * AAPI will try to queue this much per socket before shutting off flow
 * Due to latencies in shutting off slow, the real amount queued could be
 * more.
 */
#define AAPI_RFC_MAX_BUF                    16384

/*
 * Android API layer will queue up only this many work items at a time
 */
#define AAPI_NUM_OUTSTANDING_WORK_ITEMS     256

/*
 * RFC layer will buffer this many bytes for connections without credit-based flow control
 */
#define RFC_MAX_CREDITLESS_QUEUE            1024

/*
 * RFC layer will wait this many ms before closing a control connection
 * with no data channels
 */
#define RFC_USE_TIMEOUT                     2000

/*
 * RFC layer will wait this many ms before closing a control connection
 * due to response to a command timeout. Yes, this value is obscenely
 * high, but the spec mandates it be on the order of tens of seconds
 */
#define RFC_CMD_TIMEOUT                     20000

/*
 * RFC layer will give the remote side this many credits (1..7)
 */
#define RFC_NUM_REMOTE_CREDITS              7

/*
 * RFC layer will send a credit packet if we already have thsi many
 * of the other side's credits. 1 .. RFC_NUM_REMOTE_CREDITS
 */
#define RFC_THRESH_HELD_CREDITS             5

/*
 * RFC layer will queue up only this many work items at a time
 */
#define RFC_NUM_OUTSTANDING_WORK_ITEMS      256

/*
 * Default LE connection parameters
 */
#define HCI_LE_CONN_INT_MIN                10 /* 12.5 ms */
#define HCI_LE_CONN_INT_MAX                20 /* 25.0 ms */
#define HCI_LE_CONN_LATENCY                10
#define HCI_LE_CONN_TIMEOUT                83  /* .83 s */
#define HCI_LE_CONN_USE_RAND_ADDR          true
#define HCI_LE_CONN_SCAN_INTERVAL          24 /* 15 ms */
#define HCI_LE_CONN_SCAN_WINDOW            6  /* 3.75 ms */

/*
 * Default LE scan parameters
 */
#define HCI_LE_SCAN_INTERVAL               24 /* 15 ms */
#define HCI_LE_SCAN_WINDOW                 4  /* 2.5 ms */

/*
 * How long we let other side wait for our decision till we auto-reject (in units of 0.625 ms)
 */
#define HCI_CONN_RX_TIMEOUT                16000

/*
 * How long our chip should keep trying to connect to a peer (in units of 0.625 ms)
 */
#define HCI_CONN_TX_TIMEOUT                7800

/*
 * HCI layer will queue up only this many cmds at a time
 */
#define HCI_NUM_OUTSTANDING_CMDS           64

/*
 * HCI layer will queue up only this many callbacks at a time
 */
#define HCI_NUM_OUTSTANDING_CBKS           256

/*
 * L2CAP layer will allow at most this many concurrent outstanding work requests
 * to the services running on top of it / under it. If you have a lot of
 * sometimes-slow services, you may want to set this high. All requests that
 * overflow this limit will be dropped.
 */
#define L2C_SVC_NUM_OUTSTANDING_REQS       512

/*
 * L2CAP layer will allow at most this many concurrent outstanding signalling frames
 */
#define L2C_SVC_NUM_OUTSTANDING_SIG_FRAMES 512

/*
 * How long to wait before closing an unused link (in case anyone else wants it)
 * value is in msec
 */
#define L2C_ACL_LINK_TIMOUT                5000

/*
 * How long to wait giving up on opening a link
 * value is in msec
 */
#define L2C_ACL_LINK_UP_TIMEOUT            16000

/*
 * How long to wait for the other side to acknowledge our request to open an L2CAP channel
 * value is in msec
 */
#define L2C_L2CAP_OPEN_TIMEOUT             2000

/*
 * How long to wait for the other side to finish authenticating us on open
 * value is in msec
 */
#define L2C_L2CAP_OPEN_PEND_TIMEOUT        70000

/*
 * How long L2CAP configuration state has to finish configuration
 * value is in msec
 */
#define L2C_L2CAP_CONFIG_TIMEOUT           2000

/*
 * How many seconds till we decide the chip is dead
 */
#define BT_COMMAND_TIMEOUT                 10



#endif


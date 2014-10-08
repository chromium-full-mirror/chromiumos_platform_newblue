#ifndef _GATT_H_
#define _GATT_H_

/* sadly android interface stop sus from using uniq_t for handles here */

#define GATT_PSM                     0x001F
#define GATT_FIXEDCH                 0x0004



/* UUIDs & other GATT defines */
#define GATT_UUID_SVC_GAP            SDP_SVC_CLASS_GAP
#define GATT_UUID_SVC_GATT_SVR       SDP_SVC_CLASS_GATT
#define GATT_UUID_SVC_PRIMARY        0x2800
#define GATT_UUID_SVC_SECONDARY      0x2801
#define GATT_UUID_INCLUDE            0x2802
#define GATT_UUID_CHARACTERISTIC     0x2803
#define GATT_UUID_CHAR_EXTD_PROPS    0x2900
#define GATT_UUID_CHAR_USER_DESCR    0x2901
#define GATT_UUID_CHAR_CLI_CHAR_CFG  0x2902 /* uint16_t, see GATT_CLI_CFG_BIT_* */
#define GATT_UUID_CHAR_SVR_CHAR_CFG  0x2903
#define GATT_UUID_CHAR_PRES_FMT      0x2904
#define GATT_UUID_CHAR_AGGR_FMT      0x2905

#define GATT_CLI_CFG_BIT_NOTIFS      0x0001
#define GATT_CLI_CFG_BIT_INDS        0x0002

/* gatt-level perms */
#define GATT_PROP_BROADCAST          0x01
#define GATT_PROP_READ               0x02
#define GATT_PROP_WRITE_NO_RSP       0x04
#define GATT_PROP_WRITE              0x08
#define GATT_PROP_NOTIFY             0x10
#define GATT_PROP_INDICATE           0x20
#define GATT_PROP_AUTH_SIG_WRITE     0x40
#define GATT_PROP_EXTD_PROPS         0x80



#endif


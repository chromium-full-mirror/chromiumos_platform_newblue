#ifndef _HCI_INT_H_
#define _HCI_INT_H_

#include "util.h"


#define HCI_LAP_Unlimited_Inquiry                                0x9E8B33
#define HCI_LAP_Limited_Inquiry                                  0x9E8B00

#define HCI_PKT_TYP_NO_2_DH1                                     0x0002 /* BT 2.1+ */
#define HCI_PKT_TYP_NO_3_DH1                                     0x0004 /* BT 2.1+ */
#define HCI_PKT_TYP_DM1                                          0x0008 /* BT 1.1+ */
#define HCI_PKT_TYP_DH1                                          0x0010 /* BT 1.1+ */
#define HCI_PKT_TYP_NO_2_DH3                                     0x0100 /* BT 2.1+ */
#define HCI_PKT_TYP_NO_3_DH3                                     0x0200 /* BT 2.1+ */
#define HCI_PKT_TYP_DM3                                          0x0400 /* BT 1.1+ */
#define HCI_PKT_TYP_DH3                                          0x0800 /* BT 1.1+ */
#define HCI_PKT_TYP_NO_2_DH5                                     0x1000 /* BT 2.1+ */
#define HCI_PKT_TYP_NO_3_DH5                                     0x1000 /* BT 2.1+ */
#define HCI_PKT_TYP_DM5                                          0x4000 /* BT 1.1+ */
#define HCI_PKT_TYP_DH5                                          0x8000 /* BT 1.1+ */

#define HCI_PKT_TYP_SCO_HV1                                      0x0001 /* BT 1.1+ */
#define HCI_PKT_TYP_SCO_HV2                                      0x0002 /* BT 1.1+ */
#define HCI_PKT_TYP_SCO_HV3                                      0x0004 /* BT 1.1+ */
#define HCI_PKT_TYP_SCO_EV3                                      0x0008 /* BT 1.2+ */
#define HCI_PKT_TYP_SCO_EV4                                      0x0010 /* BT 1.2+ */
#define HCI_PKT_TYP_SCO_EV5                                      0x0020 /* BT 1.2+ */
#define HCI_PKT_TYP_SCO_NO_2_EV3                                 0x0040 /* BT 2.1+ */
#define HCI_PKT_TYP_SCO_NO_3_EV3                                 0x0080 /* BT 2.1+ */
#define HCI_PKT_TYP_SCO_NO_2_EV5                                 0x0100 /* BT 2.1+ */
#define HCI_PKT_TYP_SCO_NO_3_EV5                                 0x0200 /* BT 2.1+ */

#define HCI_DISPLAY_CAP_DISPLAY_ONLY                             0
#define HCI_DISPLAY_CAP_DISPLAY_YES_NO                           1
#define HCI_DISPLAY_CAP_KEYBOARD_ONLY                            2
#define HCI_DISPLAY_CAP_NO_IO                                    3

#define HCI_AUTH_REQMENT_NO_MITM_PROT_NO_BOND                    0
#define HCI_AUTH_REQMENT_MITM_PROT_NO_BOND                       1
#define HCI_AUTH_REQMENT_NO_MITM_PROT_DEDICATED_BOND             2
#define HCI_AUTH_REQMENT_MITM_PROT_DEDICATED_BOND                3
#define HCI_AUTH_REQMENT_NO_MITM_PROT_GENERAL_BOND               4
#define HCI_AUTH_REQMENT_MITM_PROT_GENERAL_BOND                  5

#define HCI_LINK_POLICY_DISABLE_ALL_LM_MODES                     0x0000
#define HCI_LINK_POLICY_ENABLE_ROLESWITCH                        0x0001
#define HCI_LINK_POLICY_ENABLE_HOLD_MODE                         0x0002
#define HCI_LINK_POLICY_ENABLE_SNIFF_MODE                        0x0004
#define HCI_LINK_POLICY_ENABLE_PARK_MODE                         0x0008

#define HCI_FILTER_TYPE_CLEAR_ALL                                0x00 /* no subtypes, no data */
#define HCI_FILTER_INQUIRY_RESULT                                0x01 /* below subtypes */
# define HCI_FILTER_COND_TYPE_RETURN_ALL_DEVS                    0x00 /* no data */
# define HCI_FILTER_COND_TYPE_SPECIFIC_DEV_CLS                   0x01 /* uint24_t wanted_class, uint24_t wanted_mask (only set bits are compared to wanted_class) */
# define HCI_FILTER_COND_TYPE_SPECIFIC_ADDR                      0x02 /* uint8_t mac[6] */
#define HCI_FILTER_CONNECTION_SETUP                              0x02 /* below subtypes */
# define HCI_FILTER_COND_TYPE_ALLOW_CONNS_FROM_ALL_DEVS          0x00 /* uint8_t auto_accept_type: 1 - no, 2 - yes w/ no roleswitch, 3 - yes w/ roleswitch */
# define HCI_FILTER_COND_TYPE_ALLOW_CONNS_FROM_SPECIFIC_DEV_CLS  0x01 /* uint24_t wanted_class, uint24_t wanted_mask (only set bits are compared to wanted_class), auto_accept flag same as above */
# define HCI_FILTER_COND_TYPE_ALLOW_CONNS_FROM_SPECIFIC_ADDR     0x02 /* uint8_t mac[6], auto_accept flag same as above */

#define HCI_SCAN_ENABLE_INQUIRY                                  0x01 /* discoverable */
#define HCI_SCAN_ENABLE_PAGE                                     0x02 /* connectable */

#define HCI_HOLD_MODE_SUSPEND_PAGE_SCAN                          0x01
#define HCI_HOLD_MODE_SUSPEND_INQUIRY_SCAN                       0x02
#define HCI_HOLD_MODE_SUSPEND_PERIODIC_INQUIRIES                 0x04

#define HCI_TO_HOST_FLOW_CTRL_ACL                                0x01
#define HCI_TO_HOST_FLOW_CTRL_SCO                                0x02

#define HCI_INQ_MODE_STD                                         0 /* normal mode @ BT 1.1+ */
#define HCI_INQ_MODE_RSSI                                        1 /* with RSSI   @ BT 1.2+ */
#define HCI_INQ_MODE_EIR                                         2 /* with EIR    @ BT 2.1+ */

#define HCI_SSP_KEY_ENTRY_STARTED                                0
#define HCI_SSP_KEY_ENTRY_DIGIT_ENTERED                          1
#define HCI_SSP_KEY_ENTRY_DIGIT_ERASED                           2
#define HCI_SSP_KEY_ENTRY_CLEARED                                3
#define HCI_SSP_KEY_ENTRY_COMPLETED                              4

#define HCI_LOCATION_DOMAIN_OPTION_NONE                          0x20 /* ' ' */
#define HCI_LOCATION_DOMAIN_OPTION_OUTDOORS_ONLY                 0x4F /* 'O' */
#define HCI_LOCATION_DOMAIN_OPTION_INDOORS_ONLY                  0x49 /* 'I' */
#define HCI_LOCATION_DOMAIN_OPTION_NON_COUNTRY_ENTITY            0x58 /* 'X' */

#define HCI_PERIOD_TYPE_DOWNLINK                                 0x00
#define HCI_PERIOD_TYPE_UPLINK                                   0x01
#define HCI_PERIOD_TYPE_BIDIRECTIONAL                            0x02
#define HCI_PERIOD_TYPE_GUARD_PERIOD                             0x03

#define HCI_MWS_INTERVAL_TYPE_NO_RX_NO_TX                        0x00
#define HCI_MWS_INTERVAL_TYPE_TX_ALLOWED                         0x01
#define HCI_MWS_INTERVAL_TYPE_RX_ALLOWED                         0x02
#define HCI_MWS_INTERVAL_TYPE_TX_RX_ALLOWED                      0x03
#define HCI_MWS_INTERVAL_TYPE_FRAME                              0x04 /* type defined by Set External Frame Configuration command */

#define HCI_CONNLESS_FRAG_TYPE_CONT                              0x00 /* continuation fragment */
#define HCI_CONNLESS_FRAG_TYPE_START                             0x01 /* first fragment */
#define HCI_CONNLESS_FRAG_TYPE_END                               0x02 /* last fragment */
#define HCI_CONNLESS_FRAG_TYPE_COMPLETE                          0x03 /* complete fragment - no fragmentation */

#define HCI_CUR_MODE_ACTIVE                                      0x00
#define HCI_CUR_MODE_HOLD                                        0x01
#define HCI_CUR_MODE_SNIFF                                       0x02
#define HCI_CUR_MODE_PARK                                        0x03

#define HCI_KEY_TYPE_COMBINATION                                 0x00 /* BT 1.1+ */
#define HCI_KEY_TYPE_LOCAL_UNIT                                  0x01 /* BT 1.1+ */
#define HCI_KEY_TYPE_REMOTE_UNIT                                 0x02 /* BT 1.1+ */
#define HCI_KEY_TYPE_DEBUG_COMBINATION                           0x03 /* BT 2.1+ */
#define HCI_KEY_TYPE_UNAUTHENTICATED_COMBO_P192                  0x04 /* BT 2.1+ */
#define HCI_KEY_TYPE_AUTHENTICATED_COMBO_P192                    0x05 /* BT 2.1+ */
#define HCI_KEY_TYPE_CHANGED_COMBO                               0x06 /* BT 2.1+ */
#define HCI_KEY_TYPE_UNAUTHENTICATED_COMBO_P256                  0x07 /* BT 4.1+ */
#define HCI_KEY_TYPE_AUTHENTICATED_COMBO_P256                    0x08 /* BT 4.1+ */

#define HCI_SCO_LINK_TYPE_SCO                                    0x00
#define HCI_SCO_LINK_TYPE_ESCO                                   0x02

#define HCI_SCO_AIR_MODE_MULAW                                   0x00
#define HCI_SCO_AIR_MODE_ALAW                                    0x01
#define HCI_SCO_AIR_MODE_CVSD                                    0x02
#define HCI_SCO_AIR_MODE_TRANSPARENT                             0x03

#define HCI_MCA_500_PPM                                          0x00
#define HCI_MCA_250_PPM                                          0x01
#define HCI_MCA_150_PPM                                          0x02
#define HCI_MCA_100_PPM                                          0x03
#define HCI_MCA_75_PPM                                           0x04
#define HCI_MCA_50_PPM                                           0x05
#define HCI_MCA_30_PPM                                           0x06
#define HCI_MCA_20_PPM                                           0x07

#define HCI_ADV_TYPE_ADV_IND                                     0x00 /* connectable undirected */
#define HCI_ADV_TYPE_ADV_DIRECT_IND                              0x01 /* connectable directed */
#define HCI_ADV_TYPE_ADV_SCAN_IND                                0x02 /* scannable undirected */
#define HCI_ADV_TYPE_ADV_NONCONN_IND                             0x03 /* non-connectable undirected */
#define HCI_ADV_TYPE_SCAN_RSP                                    0x04 /* scan response */

#define HCI_LMP_VERSION_1_0_B                                     0 /* BT 1.0b */
#define HCI_LMP_VERSION_1_1                                       1 /* BT 1.1  */
#define HCI_LMP_VERSION_1_2                                       2 /* BT 1.2  */
#define HCI_LMP_VERSION_2_0                                       4 /* BT 2.0  */
#define HCI_LMP_VERSION_2_1                                       3 /* BT 2.1  */
#define HCI_LMP_VERSION_3_0                                       4 /* BT 3.0  */
#define HCI_LMP_VERSION_4_0                                       6 /* BT 4.0  */
#define HCI_LMP_VERSION_4_1                                       7 /* BT 4.1  */

#define HCI_LMP_FTR_3_SLOT_PACKETS                               0x0000000000000001ULL /* BT 1.1+ */
#define HCI_LMP_FTR_5_SLOT_PACKETS                               0x0000000000000002ULL /* BT 1.1+ */
#define HCI_LMP_FTR_ENCRYPTION                                   0x0000000000000004ULL /* BT 1.1+ */
#define HCI_LMP_FTR_SLOT_OFFSET                                  0x0000000000000008ULL /* BT 1.1+ */
#define HCI_LMP_FTR_TIMING_ACCURACY                              0x0000000000000010ULL /* BT 1.1+ */
#define HCI_LMP_FTR_SWITCH                                       0x0000000000000020ULL /* BT 1.1+ */
#define HCI_LMP_FTR_HOLD_MODE                                    0x0000000000000040ULL /* BT 1.1+ */
#define HCI_LMP_FTR_SNIFF_MODE                                   0x0000000000000080ULL /* BT 1.1+ */
#define HCI_LMP_FTR_PARK_MODE                                    0x0000000000000100ULL /* BT 1.1+ */
#define HCI_LMP_FTR_RSSI                                         0x0000000000000200ULL /* BT 1.1+ */
#define HCI_LMP_FTR_CHANNEL_QUALITY_DRIVEN_DATA_RATE             0x0000000000000400ULL /* BT 1.1+ */
#define HCI_LMP_FTR_SCO_LINKS                                    0x0000000000000800ULL /* BT 1.1+ */
#define HCI_LMP_FTR_HV2_PACKETS                                  0x0000000000001000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_HV3_PACKETS                                  0x0000000000002000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_MU_LAW                                       0x0000000000004000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_A_LAW                                        0x0000000000008000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_CVSD                                         0x0000000000010000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_PAGING_SCHEME                                0x0000000000020000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_POWER_CONTROL                                0x0000000000040000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_TRANSPARENT_SCO_DATA                         0x0000000000080000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_FLOW_CONTROL_LAG_B0                          0x0000000000100000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_FLOW_CONTROL_LAG_B1                          0x0000000000200000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_FLOW_CONTROL_LAG_B2                          0x0000000000400000ULL /* BT 1.1+ */
#define HCI_LMP_FTR_BROADCAST_ENCRYPTION                         0x0000000000800000ULL /* BT 1.2+ */
#define HCI_LMP_FTR_ACL_2MBPS                                    0x0000000002000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_ACL_3MBPS                                    0x0000000004000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_ENHANCED_INQUIRY_SCAN                        0x0000000008000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_INTERLACED_INQUIRY_SCAN                      0x0000000010000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_INTERLACED_PAGE_SCAN                         0x0000000020000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_RSSI_WITH_INQUIRY_RESULTS                    0x0000000040000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_EXTENDED_SCO_LINK                            0x0000000080000000ULL /* BT 2.1+ */ /* EV3 packets */
#define HCI_LMP_FTR_EV4_PACKETS                                  0x0000000100000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_EV5_PACKETS                                  0x0000000200000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_AFH_CAPABLE_SLAVE                            0x0000000800000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_AFH_CLASSIFICATION_SLAVE                     0x0000001000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_3_SLOT_ACL_PACKETS                           0x0000008000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_5_SLOT_ACL_PACKETS                           0x0000010000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_SNIFF_SUBRATING                              0x0000020000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_PAUSE_ENCRYPTION                             0x0000040000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_AFH_CAPABLE_MASTER                           0x0000080000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_AFH_CLASSIFICATION_MASTER                    0x0000100000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_ESCO_2MBPS                                   0x0000200000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_ESCO_3MBPS                                   0x0000400000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_3_SLOT_ESCO                                  0x0000800000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_EXTENDED_INQUIRY_RESPONSE                    0x0001000000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_SSP                                          0x0008000000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_ENCAPSULATED_PDU                             0x0010000000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_ERRONEOUS_DATA_REPORTING                     0x0020000000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_NON_FLUSHABLE_PACKET_BOUNDARY_FLAG           0x0040000000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_LINK_SUPERVISION_TIMEOUT_CHANGED_EVENT       0x0100000000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_INQUIRY_RESPONSE_TX_POWER_LEVEL              0x0200000000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_EXTENDED_FEATURES                            0x8000000000000000ULL /* BT 2.1+ */
#define HCI_LMP_FTR_ENHANCED_POWER_CONTROL                       0x0400000000000000ULL /* BT 3.0+ */
#define HCI_LMP_FTR_SIMUL_LE_EDR_CAPABLE_CONTROLLER              0x0002000000000000ULL /* BT 4.0+ */

#define HCI_LMP_EXT_FTR_P1_SSP_HOST_SUPPORT                      0x0000000000000001ULL /* BT 2.1+ */
#define HCI_LMP_EXT_FTR_P1_LE_HOST_SUPPORT                       0x0000000000000002ULL /* BT 4.0+ */
#define HCI_LMP_EXT_FTR_P1_SIMUL_LE_EDR_HOST_SUPPORT             0x0000000000000004ULL /* BT 4.0+ */
#define HCI_LMP_EXT_FTR_P1_SECURE_CONNECTIONS_HOST_SUPPORT       0x0000000000000008ULL /* BT 4.1+ */

#define HCI_LMP_EXT_FTR_P2_CONNLESS_SLAVE_BROADCAST_MASTER       0x0000000000000001ULL /* BT 4.1+ */
#define HCI_LMP_EXT_FTR_P2_CONNLESS_SLAVE_BROADCAST_SLAVE        0x0000000000000002ULL /* BT 4.1+ */
#define HCI_LMP_EXT_FTR_P2_SYNCHRONIZATION_TRAIN                 0x0000000000000004ULL /* BT 4.1+ */
#define HCI_LMP_EXT_FTR_P2_SYNCHRONIZATION_SCAN                  0x0000000000000008ULL /* BT 4.1+ */
#define HCI_LMP_EXT_FTR_P2_INQUIRY_RESPONSE_NOTIFICATION_EVT     0x0000000000000010ULL /* BT 4.1+ */
#define HCI_LMP_EXT_FTR_P2_GENERALIZED_INTERLACED_SCAN           0x0000000000000020ULL /* BT 4.1+ */
#define HCI_LMP_EXT_FTR_P2_COARSE_CLOCK_ADJUSTMENT               0x0000000000000040ULL /* BT 4.1+ */
#define HCI_LMP_EXT_FTR_P2_SECURE_CONNECTIONS_CAPABLE_CONTROLLER 0x0000000000000100ULL /* BT 4.1+ */
#define HCI_LMP_EXT_FTR_P2_PING                                  0x0000000000000200ULL /* BT 4.1+ */
#define HCI_LMP_EXT_FTR_P2_TRAIN_NUDGING                         0x0000000000000800ULL /* BT 4.1+ */

#define HCI_EVENT_INQUIRY_COMPLETE                               0x0000000000000001ULL /* BT 1.1+ */
#define HCI_EVENT_INQUIRY_RESULT                                 0x0000000000000002ULL /* BT 1.1+ */
#define HCI_EVENT_CONN_COMPLETE                                  0x0000000000000004ULL /* BT 1.1+ */
#define HCI_EVENT_CONN_REQUEST                                   0x0000000000000008ULL /* BT 1.1+ */
#define HCI_EVENT_DISCONNECTION_COMPLETE                         0x0000000000000010ULL /* BT 1.1+ */
#define HCI_EVENT_AUTH_COMPLETE                                  0x0000000000000020ULL /* BT 1.1+ */
#define HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE                   0x0000000000000040ULL /* BT 1.1+ */
#define HCI_EVENT_ENCR_CHANGE                                    0x0000000000000080ULL /* BT 1.1+ */
#define HCI_EVENT_CHANGE_CONN_LINK_KEY_COMPLETE                  0x0000000000000100ULL /* BT 1.1+ */
#define HCI_EVENT_MASTER_LINK_KEY_COMPLETE                       0x0000000000000200ULL /* BT 1.1+ */
#define HCI_EVENT_READ_REMOTE_SUPPORTED_FEATURES_COMPLETE        0x0000000000000400ULL /* BT 1.1+ */
#define HCI_EVENT_READ_REMOTE_VERSON_INFO_COMPLETE               0x0000000000000800ULL /* BT 1.1+ */
#define HCI_EVENT_QOS_SETUP_COMPLETE                             0x0000000000001000ULL /* BT 1.1+ */
#define HCI_EVENT_HARDWARE_ERROR                                 0x0000000000008000ULL /* BT 1.1+ */
#define HCI_EVENT_FLUSH_OCCURED                                  0x0000000000010000ULL /* BT 1.1+ */
#define HCI_EVENT_ROLE_CHANGE                                    0x0000000000020000ULL /* BT 1.1+ */
#define HCI_EVENT_MODE_CHANGE                                    0x0000000000080000ULL /* BT 1.1+ */
#define HCI_EVENT_RETURN_LINK_KEYS                               0x0000000000100000ULL /* BT 1.1+ */
#define HCI_EVENT_PIN_CODE_REQUEST                               0x0000000000200000ULL /* BT 1.1+ */
#define HCI_EVENT_LINK_KEY_REQUEST                               0x0000000000400000ULL /* BT 1.1+ */
#define HCI_EVENT_LINK_KEY_NOTIFICATION                          0x0000000000800000ULL /* BT 1.1+ */
#define HCI_EVENT_LOOPBACK_COMMAND                               0x0000000001000000ULL /* BT 1.1+ */
#define HCI_EVENT_DATA_BUFFER_OVERFLOW                           0x0000000002000000ULL /* BT 1.1+ */
#define HCI_EVENT_MAX_SLOTS_CHANGE                               0x0000000004000000ULL /* BT 1.1+ */
#define HCI_EVENT_READ_CLOCK_OFFSET_COMPLETE                     0x0000000008000000ULL /* BT 1.1+ */
#define HCI_EVENT_CONN_PACKET_TYPE_CHANGED                       0x0000000010000000ULL /* BT 1.1+ */
#define HCI_EVENT_QOS_VIOLATION                                  0x0000000020000000ULL /* BT 1.1+ */
#define HCI_EVENT_PAGE_SCAN_MODE_CHANGE                          0x0000000040000000ULL /* BT 1.1+, obsolete @ BT1.2+ */
#define HCI_EVENT_PAGE_SCAN_REPETITION_MODE_CHANGE               0x0000000080000000ULL /* BT 1.1+ */
#define HCI_EVENT_ALL_BT_1_1                                     0x00000000FFFFFFFFULL /* also the default for BT 1.1 */
#define HCI_EVENT_FLOW_SPEC_COMPLETE                             0x0000000100000000ULL /* BT 1.2+ */
#define HCI_EVENT_INQUIRY_RESULT_WITH_RSSI                       0x0000000200000000ULL /* BT 1.2+ */
#define HCI_EVENT_READ_REMOTE_EXTENDED_FEATURES_COMPLETE         0x0000000400000000ULL /* BT 1.2+ */
#define HCI_EVENT_SYNC_CONN_COMPLETE                             0x0000080000000000ULL /* BT 1.2+ */
#define HCI_EVENT_SYNC_CONN_CHANGED                              0x0000100000000000ULL /* BT 1.2+ */
#define HCI_EVENT_ALL_BT_1_2                                     0x00001FFFFFFFFFFFULL /* also the default for BT 1.2+ */
#define HCI_EVENT_SNIFF_SUBRATING                                0x0000200000000000ULL /* BT 2.1+ */
#define HCI_EVENT_EXTENDED_INQUIRY_RESULT                        0x0000400000000000ULL /* BT 2.1+ */
#define HCI_EVENT_ENCR_KEY_REFRESH_COMPLETE                      0x0000800000000000ULL /* BT 2.1+ */
#define HCI_EVENT_IO_CAPABILITY_REQUEST                          0x0001000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_IO_CAPABILITY_REQUEST_REPLY                    0x0002000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_USER_CONFIRMATION_REQUEST                      0x0004000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_USER_PASSKEY_REQUEST                           0x0008000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_REMOTE_OOB_DATA_REQUEST                        0x0010000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_SIMPLE_PAIRING_COMPLETE                        0x0020000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_LINK_SUPERVISION_TIMOUT_CHANGED                0x0080000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_ENHANCED_FLUSH_COMPLETE                        0x0100000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_USER_PASSKEY_NOTIFICATION                      0x0400000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_KEYPRESS_NOTIFICATION                          0x0800000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_REMOTE_HOST_SUPPORTED_FEATURES                 0x1000000000000000ULL /* BT 2.1+ */
#define HCI_EVENT_ALL_BT_2_1                                     0x1DBFFFFFFFFFFFFFULL
#define HCI_EVENT_ALL_BT_3_0                                     0x1DBFFFFFFFFFFFFFULL
#define HCI_EVENT_LE_META                                        0x2000000000000000ULL /* BT 4.0+ */
#define HCI_EVENT_ALL_BT_4_0                                     0x3DBFFFFFFFFFFFFFULL

#define HCI_EVENT_P2_PHYS_LINK_COMPLETE                          0x0000000000000001ULL /* BT 3.0+ */
#define HCI_EVENT_P2_CHANNEL_SELECTED                            0x0000000000000002ULL /* BT 3.0+ */
#define HCI_EVENT_P2_DISCONNECTION_PHYSICAL_LINK                 0x0000000000000004ULL /* BT 3.0+ */
#define HCI_EVENT_P2_PHYSICAL_LINK_LOSS_EARLY_WARNING            0x0000000000000008ULL /* BT 3.0+ */
#define HCI_EVENT_P2_PHYSICAL_LINK_RECOVERY                      0x0000000000000010ULL /* BT 3.0+ */
#define HCI_EVENT_P2_LOGICAL_LINK_COMPLETE                       0x0000000000000020ULL /* BT 3.0+ */
#define HCI_EVENT_P2_FLOW_SPEC_MODIFY_COMPLETE                   0x0000000000000080ULL /* BT 3.0+ */
#define HCI_EVENT_P2_NUMBER_OF_COMPLETED_DATA_BLOCKS             0x0000000000000100ULL /* BT 3.0+ */
#define HCI_EVENT_P2_AMP_START_TEST                              0x0000000000000200ULL /* BT 3.0+ */
#define HCI_EVENT_P2_AMP_TEST_END                                0x0000000000000400ULL /* BT 3.0+ */
#define HCI_EVENT_P2_AMP_RECEIVER_REPORT                         0x0000000000000800ULL /* BT 3.0+ */
#define HCI_EVENT_P2_SHORT_RANGE_MODE_CHANGE_COMPLETE            0x0000000000001000ULL /* BT 3.0+ */
#define HCI_EVENT_P2_AMP_STATUS_CHANGE                           0x0000000000002000ULL /* BT 3.0+ */
#define HCI_EVENT_P2_TRIGGERED_CLOCK_CAPTURE                     0x0000000000004000ULL /* BT 4.1+ */
#define HCI_EVENT_P2_SYNCH_TRAIN_COMPLETE                        0x0000000000008000ULL /* BT 4.1+ */
#define HCI_EVENT_P2_SYNCH_TRAIN_RECEIVED                        0x0000000000010000ULL /* BT 4.1+ */
#define HCI_EVENT_P2_CONNLESS_SLAVE_BROADCAST_RXED               0x0000000000020000ULL /* BT 4.1+ */
#define HCI_EVENT_P2_CONNLESS_SLAVE_BROADCAST_TIMEOUT            0x0000000000040000ULL /* BT 4.1+ */
#define HCI_EVENT_P2_TRUNCATED_PAGE_COMPLETE                     0x0000000000080000ULL /* BT 4.1+ */
#define HCI_EVENT_P2_SLAVE_PAGE_RESPONSE_TIMEOUT                 0x0000000000100000ULL /* BT 4.1+ */
#define HCI_EVENT_P2_CONNLESS_SLAVE_BROADCAST_CHANNEL_MAP_CHANGE 0x0000000000200000ULL /* BT 4.1+ */
#define HCI_EVENT_P2_INQUIRY_RESPONSE_NOTIFICATION               0x0000000000400000ULL /* BT 4.1+ */
#define HCI_EVENT_P2_AUTHENTICATED_PAYLOAD_TIMEOUT_EXPIRED       0x0000000000800000ULL /* BT 4.1+ */

#define HCI_LE_EVENT_CONN_COMPLETE                               0x0000000000000001ULL /* BT 4.0+ */
#define HCI_LE_EVENT_ADV_REPORT                                  0x0000000000000002ULL /* BT 4.0+ */
#define HCI_LE_EVENT_CONN_UPDATE_COMPLETE                        0x0000000000000004ULL /* BT 4.0+ */
#define HCI_LE_EVENT_READ_REMOTE_USED_FEATURES_CMPLETE           0x0000000000000008ULL /* BT 4.0+ */
#define HCI_LE_EVENT_LTK_REQUEST                                 0x0000000000000010ULL /* BT 4.0+ */
#define HCI_LE_EVENT_REMOTE_CONNECTION_PARAMETER_REQUEST         0x0000000000000020ULL /* BT 4.1+ */

#define HCI_LE_FTR_ENCRYPTION                                    0x0000000000000001ULL /* BT 4.0+ */
#define HCI_LE_FTR_CONNECTION_PARAMETERS_REQUEST                 0x0000000000000002ULL /* BT 4.1+ */
#define HCI_LE_FTR_EXTENDED_REJECT_INDICATION                    0x0000000000000004ULL /* BT 4.1+ */
#define HCI_LE_FTR_SLAVE_INITIATED_FEATURES_EXCHANGE             0x0000000000000008ULL /* BT 4.1+ */
#define HCI_LE_FTR_LE_PING                                       0x0000000000000010ULL /* BT 4.1+ */





#define HCI_OGF_Link_Control                                     1


/* ==== BT 1.1 ==== */

#define HCI_CMD_Inquiry                                          0x0001
struct hciInquiry {
    uint8_t lap[3];
    uint8_t inqLen;
    uint8_t numResp;
} __packed;

#define HCI_CMD_Inquiry_Cancel                                   0x0002

#define HCI_CMD_Periodic_Inquiry_Mode                            0x0003
struct hciPeriodicInquiryMode {
    uint16_t maxPeriodLen;
    uint16_t minPeriodLen;
    uint8_t lap[3];
    uint8_t inqLen;
    uint8_t numResp;
} __packed;

#define HCI_CMD_Exit_Periodic_Inquiry_Mode                       0x0004

#define HCI_CMD_Create_Connection                                0x0005
struct hciCreateConnection {
    uint8_t mac[6];
    uint16_t allowedPackets; /* HCI_PKT_TYP_* */
    uint8_t PSRM;
    uint16_t clockOffset; /* possibly | HCI_CLOCK_OFST_VALID */
    uint8_t allowRoleSwitch;
} __packed;
#define HCI_CLOCK_OFST_VALID  0x8000

#define HCI_CMD_Disconnect                                       0x0006
struct hciDisconnect {
    uint16_t conn;
    uint8_t reason;
} __packed;

#define HCI_CMD_Add_SCO_Connection                               0x0007 /* deprecated in BT 1.2+ */
struct hciAddScoConnection {
    uint16_t conn;
    uint16_t packetTypes; /* HCI_PKT_TYP_SCO_* */
} __packed;

#define HCI_CMD_Create_Connection_Cancel                         0x0008
struct hciCreateConnectionCancel {
    uint8_t mac[6];
} __packed;

#define HCI_CMD_Accept_Connection_Request                        0x0009
struct hciAcceptConnection {
    uint8_t mac[6];
    uint8_t remainSlave;
} __packed;

#define HCI_CMD_Reject_Connection_Request                        0x000A
struct hciRejectConnection {
    uint8_t mac[6];
    uint8_t reason;
} __packed;

#define HCI_CMD_Link_Key_Request_Reply                           0x000B
struct hciLinkKeyRequestReply {
    uint8_t mac[6];
    uint8_t key[16];
} __packed;

#define HCI_CMD_Link_Key_Request_Negative_Reply                  0x000C
struct hciLinkKeyRequestNegativeReply {
    uint8_t mac[6];
} __packed;

#define HCI_CMD_PIN_Code_Request_Reply                           0x000D
struct hciPinCodeRequestReply {
    uint8_t mac[6];
    uint8_t pinCodeLen;
    uint8_t pinCode[16];
} __packed;

#define HCI_CMD_PIN_Code_Request_Negative_Reply                  0x000E
struct hciPinCodeRequestNegativeReply {
    uint8_t mac[6];
} __packed;

#define HCI_CMD_Change_Connection_Packet_Type                    0x000F
struct hciChangeConnectionPacketType {
    uint16_t conn;
    uint16_t allowedPackets; /* HCI_PKT_TYP_* */
} __packed;

#define HCI_CMD_Authentication_Requested                         0x0011
struct hciAuthRequested {
    uint16_t conn;
} __packed;

#define HCI_CMD_Set_Connection_Encryption                        0x0013
struct hciSetConnectionEncryption {
    uint16_t conn;
    uint8_t encrOn;
} __packed;

#define HCI_CMD_Change_Connection_Link_Key                       0x0015
struct hciChangeConnLinkKey {
    uint16_t conn;
} __packed;

#define HCI_CMD_Master_Link_Key                                  0x0017
struct hciMasterLinkKey {
    uint8_t useTempKey;
} __packed;

#define HCI_CMD_Remote_Name_Request                              0x0019
struct hciNameRequest {
    uint8_t mac[6];
    uint8_t PSRM;
    uint8_t PSM; /* deprecated, should be zero for BT 1.2+ */
    uint16_t clockOffset; /* possibly | HCI_CLOCK_OFST_VALID */
} __packed;

#define HCI_CMD_Remote_Name_Request_Cancel                       0x001A
struct hciNameRequestCancel {
    uint8_t mac[6];
} __packed;

#define HCI_CMD_Read_Remote_Supported_Features                   0x001B
struct hciReadRemoteSupportedFeatures {
    uint16_t conn;
} __packed;

#define HCI_CMD_Read_Remote_Extended_Features                    0x001C
struct hciReadRemoteExtendedFeatures {
    uint16_t conn;
    uint8_t page; /* BT1.2 max: 0 */
} __packed;

#define HCI_CMD_Read_Remote_Version_Information                  0x001D
struct hciReadRemoteVersionInfo {
    uint16_t conn;
} __packed;

#define HCI_CMD_Read_Clock_Offset                                0x001F
struct hciReadClockOffset {
    uint16_t conn;
} __packed;


/* ==== BT 1.2 ==== */

#define HCI_CMD_Read_Lmp_Handle                                  0x0020
struct hciReadLmpHandle {
    uint16_t handle;
} __packed;

#define HCI_CMD_Setup_Synchronous_Connection                     0x0028
struct hciSetupSyncConn {
    uint16_t conn;
    uint32_t txBandwidth;
    uint32_t rxBandwidth;
    uint16_t maxLatency;
    uint16_t voiceSetting;
    uint8_t retransmissionEffort;
    uint16_t allowedPacketsSco; /* HCI_PKT_TYP_SCO_* */
} __packed;

#define HCI_CMD_Accept_Synchronous_Connection_Request            0x0029
struct hciAcceptSyncConn {
    uint8_t mac[6];
    uint32_t txBandwidth;
    uint32_t rxBandwidth;
    uint16_t maxLatency;
    uint16_t contentFormat;
    uint8_t retransmissionEffort;
    uint16_t allowedPacketsSco; /* HCI_PKT_TYP_SCO_* */
} __packed;

#define HCI_CMD_Reject_Synchronous_Connection_Request            0x002A
struct hciRejectSyncConn {
    uint8_t mac[6];
    uint8_t reason;
} __packed;


/* ==== BR 2.1 ==== */

#define HCI_CMD_IO_Capability_Request_Reply                      0x002B
struct hciIoCapabilityRequestReply {
    uint8_t mac[6];
    uint8_t cap; /* HCI_DISPLAY_CAP_* */
    uint8_t oobPresent;
    uint8_t authReqments; /* HCI_AUTH_REQMENT_* */
} __packed;

#define HCI_CMD_User_Confirmation_Request_Reply                  0x002C
struct hciUserConfRequestReply {
    uint8_t mac[6];
} __packed;

#define HCI_CMD_User_Confirmation_Request_Negative_Reply         0x002D
struct hciUserConfRequestNegativeReply {
    uint8_t mac[6];
} __packed;

#define HCI_CMD_User_Passkey_Request_Reply                       0x002E
struct hciUserPasskeyRequestReply {
    uint8_t mac[6];
    uint32_t num;
} __packed;

#define HCI_CMD_User_Passkey_Request_Negative_Reply              0x002F
struct hciUserPasskeyRequestNegativeReply {
    uint8_t mac[6];
} __packed;

#define HCI_CMD_Remote_OOB_Data_Request_Reply                    0x0030
struct hciRemoteOobDataRequestReply {
    uint8_t mac[6];
    uint8_t C[16];
    uint8_t R[16];
} __packed;

#define HCI_CMD_Remote_OOB_Data_Request_Negative_Reply           0x0033
struct hciOobDataRequestNegativeReply {
    uint8_t mac[6];
} __packed;

#define HCI_CMD_IO_Capability_Request_Negative_Reply             0x0034
struct hciIoCapabilityRequestNegativeReply {
    uint8_t mac[6];
    uint8_t reason;
} __packed;


/* ==== BT 3.0 ==== */

#define HCI_CMD_Create_Physical_link                             0x0035
struct hciCreatePhysicalLink {
    uint8_t physLinkHandle;
    uint8_t dedicatedAmpKeyLength;
    uint8_t dedicatedAmpKeyType;
    uint8_t dedicatedAmpKey;
} __packed;

#define HCI_CMD_Accept_Physical_link                             0x0036
struct hciAcceptPhysicalLink {
    uint8_t physLinkHandle;
    uint8_t dedicatedAmpKeyLength;
    uint8_t dedicatedAmpKeyType;
    uint8_t dedicatedAmpKey;
} __packed;

#define HCI_CMD_Disconnect_Physical_link                         0x0037
struct hciDisconnectPhysicalLink {
    uint8_t physLinkHandle;
    uint8_t reason;
} __packed;

#define HCI_CMD_Create_Logical_link                              0x0038
struct hciCreateLogicalLink {
    uint8_t physLinkHandle;
    uint8_t txFlowSpec[16];
    uint8_t rxFlowSpec[16];
} __packed;

#define HCI_CMD_Accept_Logical_Link                              0x0039
struct hciAcceptLogicalLink {
    uint8_t physLinkHandle;
    uint8_t txFlowSpec[16];
    uint8_t rxFlowSpec[16];
} __packed;

#define HCI_CMD_Disconnect_Logical_link                          0x003A
struct hciDisconnectLogicalLink {
    uint8_t physLinkHandle;
} __packed;

#define HCI_CMD_Logical_Link_Cancel                              0x003B
struct hciLogicalLinkCancel {
    uint8_t physLinkHandle;
    uint8_t txFlowSpecID;
} __packed;

#define HCI_CMD_Flow_Spec_Modify                                 0x003C
struct hciFlowSpecModify {
    uint16_t handle;
    uint8_t txFlowSpec[16];
    uint8_t rxFlowSpec[16];
} __packed;


/* ==== BT 4.1 ==== */

#define HCI_CMD_Enhanced_Setup_Synchronous_Connection            0x003D
struct hciEnhSetupSyncConn {
    uint16_t conn;
    uint32_t txBandwidth;
    uint32_t rxBandwidth;
    uint8_t txCodingFormat[5];
    uint8_t rxCodingFormat[5];
    uint16_t txCodecFrameSize;
    uint16_t rxCodecFrameSize;
    uint32_t inputBandwidth;
    uint32_t outputBandwidth;
    uint8_t inputCodingFormat[5];
    uint8_t outputCodingFormat[5];
    uint16_t inputCodedDataSize;
    uint16_t outputCodedDataSize;
    uint8_t inputPcmDataFormat;
    uint8_t outputPcmDataFormat;
    uint8_t inputPcmSamplePayloadMsbPosition;
    uint8_t outputPcmSamplePayloadMsbPosition;
    uint8_t inputDataPath;
    uint8_t outputDataPath;
    uint8_t inputTransportUnitSize;
    uint8_t outputTransportUnitSize;
    uint16_t maxLatency;
    uint16_t allowedPacketsSco; /* HCI_PKT_TYP_SCO_* */
    uint8_t retransmissionEffort;
} __packed;

#define HCI_CMD_Enhanced_Accept_Synchronous_Connection           0x003E
struct hciEnhAcceptSyncConn {
    uint8_t mac[6];
    uint32_t txBandwidth;
    uint32_t rxBandwidth;
    uint8_t txCodingFormat[5];
    uint8_t rxCodingFormat[5];
    uint16_t txCodecFrameSize;
    uint16_t rxCodecFrameSize;
    uint32_t inputBandwidth;
    uint32_t outputBandwidth;
    uint8_t inputCodingFormat[5];
    uint8_t outputCodingFormat[5];
    uint16_t inputCodedDataSize;
    uint16_t outputCodedDataSize;
    uint8_t inputPcmDataFormat;
    uint8_t outputPcmDataFormat;
    uint8_t inputPcmSamplePayloadMsbPosition;
    uint8_t outputPcmSamplePayloadMsbPosition;
    uint8_t inputDataPath;
    uint8_t outputDataPath;
    uint8_t inputTransportUnitSize;
    uint8_t outputTransportUnitSize;
    uint16_t maxLatency;
    uint16_t allowedPacketsSco; /* HCI_PKT_TYP_SCO_* */
    uint8_t retransmissionEffort;
} __packed;

#define HCI_CMD_Truncated_Page                                   0x003F
struct hciTruncatedPage {
    uint8_t mac[6];
    uint8_t PSRM;
    uint16_t clockOffset; /* possibly | HCI_CLOCK_OFST_VALID */
} __packed;

#define HCI_CMD_Truncated_Page_Cancel                            0x0040
struct hciTruncatedPageCancel {
    uint8_t mac[6];
} __packed;

#define HCI_CMD_Set_Connectionless_Slave_Broadcast               0x0041
struct hciSetConnectionlessSlaveBroadcast {
    uint8_t enabled;
    uint8_t ltAddr; /* 1..7 */
    uint8_t lpoAllowed; /* can sleep? */
    uint16_t allowedPackets; /* HCI_PKT_TYP_* */
    uint16_t intervalMin;
    uint16_t intervalMax;
    uint16_t supervisionTimeout;
} __packed;


#define HCI_CMD_Set_Connectionless_Slave_Broadcast_Receive       0x0042
struct hciSetConnectionlessSlaveBroadcastReceive {
    uint8_t enabled;
    uint8_t mac[6]; /* add rof tranmitter */
    uint8_t ltAddr; /* 1..7 */
    uint16_t interval;
    uint32_t clockOffset; /* lower 28 bits used */
    uint32_t nextConnectionlessSlaveBroadcastClock; /* lower 28 bits used */
    uint16_t supervisionTimeout;
    uint8_t remoteTimingAccuracy;
    uint8_t skip;
    uint16_t allowedPackets; /* HCI_PKT_TYP_* */
    uint8_t afhChannelMap[10];
} __packed;

#define HCI_CMD_Start_Synchronisation_Train                      0x0043

#define HCI_CMD_Receive_Synchronisation_Train                    0x0044
struct hciReceiveSyncTrain {
    uint8_t mac[6];
    uint16_t syncScanTimeout;
    uint16_t syncScanWindow;
    uint16_t syncScanInterval;
} __packed;

#define HCI_CMD_Remote_OOB_Extended_Data_Request_Reply           0x0045
struct hciRemoteOobExtendedDataRequestReply {
    uint8_t mac[6];
    uint8_t C_192[16];
    uint8_t R_192[16];
    uint8_t C_256[16];
    uint8_t R_256[16];
} __packed;



#define HCI_OGF_Link_Policy                                      2


/* ==== BT 1.1 ==== */

#define HCI_CMD_Hold_Mode                                        0x0001
struct hciHoldMode {
    uint16_t conn;
    uint16_t holdModeMaxInt;
    uint16_t holdModeMinInt;
} __packed;

#define HCI_CMD_Sniff_Mode                                       0x0003
struct hciSniffMode {
    uint16_t conn;
    uint16_t sniffMaxInt;
    uint16_t sniffMinInt;
    uint16_t sniffAttempt;
    uint16_t sniffTimeout;
} __packed;

#define HCI_CMD_Exit_Sniff_Mode                                  0x0004
struct hciExitSniffMode {
    uint16_t conn;
} __packed;

#define HCI_CMD_Park_State                                       0x0005
struct hciParkState {
    uint16_t conn;
    uint16_t beaconMaxInt;
    uint16_t beaconMinInt;
} __packed;

#define HCI_CMD_Exit_Park_State                                  0x0006
struct hciExitParkState {
    uint16_t conn;
} __packed;

#define HCI_CMD_QoS_Setup                                        0x0007
struct hisQosSetup {
    uint16_t conn;
    uint8_t flags;
    uint8_t serviceType;
    uint32_t tokenRate;
    uint32_t peakBandwidth;
    uint32_t latency;
    uint32_t delayVariation;
} __packed;

#define HCI_CMD_Role_Discovery                                   0x0009
struct hciRoleDiscovery {
    uint16_t conn;
} __packed;

#define HCI_CMD_Switch_Role                                      0x000B
struct hciSwitchRole {
    uint8_t mac[6];
    uint8_t becomeSlave;
} __packed;

#define HCI_CMD_Read_Link_Policy_Settings                        0x000C
struct hciReadLinkPolicySettings {
    uint16_t conn;
} __packed;

#define HCI_CMD_Write_Link_Policy_Settings                       0x000D
struct hciWriteLinkPolicySettings {
    uint16_t conn;
    uint16_t policy; /* HCI_LINK_POLICY_* */
} __packed;


/* ==== BT 1.2 ==== */

#define HCI_CMD_Read_Default_Link_Policy_Settings                0x000E

#define HCI_CMD_Write_Default_Link_Policy_Settings               0x000F
struct hciWriteDefaultLinkPolicySettings {
    uint16_t policy; /* HCI_LINK_POLICY_* */
} __packed;

#define HCI_CMD_Flow_Specification                               0x0010
struct hisFlowSpecification {
    uint16_t conn;
    uint8_t flags;
    uint8_t flowDirection;
    uint8_t serviceType;
    uint32_t tokenRate;
    uint32_t tockenBucketSize;
    uint32_t peakBandwidth;
    uint32_t accessLatency;
} __packed;


/* ==== BT 2.1 ==== */

#define HCI_CMD_Sniff_Subrating                                  0x0011
struct hciSniffSubrating {
    uint16_t conn;
    uint16_t maxLatency;
    uint16_t minRemoteTimeout;
    uint16_t minLocalTimeout;
} __packed;





#define HCI_OGF_Controller_and_Baseband                          3


/* ==== BT 1.1 ==== */

#define HCI_CMD_Set_Event_Mask                                   0x0001
struct hciCmdSetEventMask {
    uint64_t mask; /* bitmask of HCI_EVENT_* */
} __packed;

#define HCI_CMD_Reset                                            0x0003

#define HCI_CMD_Set_Event_Filter                                 0x0005
struct hciSetEventFilter {
    uint8_t filterType; /* HCI_FILTER_TYPE_* */
    /* more things are optional here */
} __packed;

#define HCI_CMD_Flush                                            0x0008
struct hciFlush {
    uint16_t conn;
} __packed;

#define HCI_CMD_Read_PIN_Type                                    0x0009

#define HCI_CMD_Write_PIN_Type                                   0x000A
struct hciWritePinType {
    uint8_t isFixed;
} __packed;

#define HCI_CMD_Create_New_Unit_Key                              0x000B

#define HCI_CMD_Read_Stored_Link_Key                             0x000D
struct hciReadStoredLinkKey {
    uint8_t mac[6];
    uint8_t readAll;
} __packed;

#define HCI_CMD_Write_Stored_Link_Key                            0x0011
struct hciWriteStoredLinkKeyItem {
    uint8_t mac[6];
    uint8_t key[16];
} __packed;
struct hciWriteStoredLinkKey {
    uint8_t numKeys;
    struct hciWriteStoredLinkKeyItem items[];
} __packed;

#define HCI_CMD_Delete_Stored_Link_Key                           0x0012
struct hciDelteStoredLinkKey {
    uint8_t mac[6];
    uint8_t deleteAll;
} __packed;

#define HCI_CMD_Write_Local_Name                                 0x0013
struct hciCmdWriteLocalName {
    char name[248];
} __packed;

#define HCI_CMD_Read_Local_Name                                  0x0014

#define HCI_CMD_Read_Connection_Accept_Timeout                   0x0015

#define HCI_CMD_Write_Connection_Accept_Timeout                  0x0016
struct hciWriteConnAcceptTimeout {
    uint16_t timeout; /* in units of 0.625ms 1..0xB540 */
} __packed;

#define HCI_CMD_Read_Page_Timeout                                0x0017

#define HCI_CMD_Write_Page_Timeout                               0x0018
struct hciWritePageTimeout {
    uint16_t timeout;
} __packed;

#define HCI_CMD_Read_Scan_Enable                                 0x0019

#define HCI_CMD_Write_Scan_Enable                                0x001A
struct hciWriteScanEnable {
    uint8_t state; /* bitmask of HCI_SCAN_ENABLE_* */
} __packed;

#define HCI_CMD_Read_Page_Scan_Activity                          0x001B

#define HCI_CMD_Write_Page_Scan_Activity                         0x001C
struct hciWritePageScanActivity {
    uint16_t scanInterval;
    uint16_t scanWindow;
} __packed;

#define HCI_CMD_Read_Inquiry_Scan_Activity                       0x001D

#define HCI_CMD_Write_Inquiry_Scan_Activity                      0x001E
struct hciWriteInquiryScanActivity {
    uint16_t scanInterval;
    uint16_t scanWindow;
} __packed;

#define HCI_CMD_Read_Authentication_Enable                       0x001F

#define HCI_CMD_Write_Authentication_Enable                      0x0020
struct hciWriteAuthEnable {
    uint8_t authRequired;
} __packed;

#define HCI_CMD_Read_Encryption_Mode                             0x0021

#define HCI_CMD_Write_Encryption_Mode                            0x0022
struct hciWriteencryptionMode {
    uint8_t encrRequired;
} __packed;

#define HCI_CMD_Read_Class_Of_Device                             0x0023

#define HCI_CMD_Write_Class_Of_Device                            0x0024
struct hciCmdWriteClassOfDevice {
    uint8_t cls[3];
} __packed;

#define HCI_CMD_Read_Voice_Setting                               0x0025

#define HCI_CMD_Write_Voice_Setting                              0x0026
struct hciWriteVoiceSetting {
    uint16_t voiceSetting;
} __packed;

#define HCI_CMD_Read_Automatic_Flush_Timeout                     0x0027
struct hciReadAutoFlushTimeout {
    uint16_t conn;
} __packed;

#define HCI_CMD_Write_Automatic_Flush_Timeout                    0x0028
struct hciWriteAutoFlushTimeout {
    uint16_t conn;
    uint16_t timeout;
} __packed;

#define HCI_CMD_Read_Num_Broadcast_Retransmissions               0x0029

#define HCI_CMD_Write_Num_Broadcast_Retransmissions              0x002A
struct hciWriteNumBroadcastRetransmissions {
    uint8_t numRetransmissions; /* 0 .. 0xFE => 1 .. 255 TXes */
} __packed;

#define HCI_CMD_Read_Hold_Mode_Activity                          0x002B

#define HCI_CMD_Write_Hold_Mode_Activity                         0x002C
struct hciWriteHoldModeActivity {
    uint8_t holdModeActivity; /* bitfield if HCI_HOLD_MODE_SUSPEND_* */
} __packed;

#define HCI_CMD_Read_Transmit_Power_Level                        0x002D
struct hciReadTransmitPowerLevel {
    uint16_t conn;
    uint8_t max; /* else current */
} __packed;

#define HCI_CMD_Read_SCO_Flow_Control_Enable                     0x002E

#define HCI_CMD_Write_SCO_Flow_Control_Enable                    0x002F
struct hciWriteSyncFlowCtrl {
    uint8_t syncFlowCtrlOn;
} __packed;

#define HCI_CMD_Set_Controller_To_Host_Flow_Control              0x0031
struct hciSetControllerToHostFlowControl {
    uint8_t chipToHostFlowCtrl; /* bitmask of HCI_TO_HOST_FLOW_CTRL_* */
} __packed;

#define HCI_CMD_Host_Buffer_Size                                 0x0033
struct hciHostBufferSize {
    uint16_t maxAclPacket;
    uint8_t maxScoPacket;
    uint16_t numAclPackets;
    uint16_t numScoPackets;
} __packed;

#define HCI_CMD_Host_Number_Of_Completed_Packets                 0x0035
struct hciHostNumberOfCompletedPacketsItem {
    uint16_t conn;
    uint16_t numCompletedPackets;
} __packed;
struct hciHostNumberOfCompletedPackets {
    uint8_t numHandles;
    struct hciHostNumberOfCompletedPacketsItem items[];
} __packed;

#define HCI_CMD_Read_Link_Supervision_Timeout                    0x0036
struct hciReadLinkSupervisionTimeout {
    uint16_t conn;
} __packed;

#define HCI_CMD_Write_Link_Supervision_Timeout                   0x0037
struct hciWriteLinkSupervisionTimeout {
    uint16_t conn;
    uint16_t timeout; /* in units of 0.625ms allowed: 1..0xffff, required support 0x0190 - 0xffff */
} __packed;

#define HCI_CMD_Read_Number_Of_Supported_IAC                     0x0038

#define HCI_CMD_Read_Current_IAC_LAP                             0x0039

#define HCI_CMD_Write_Current_IAC_LAP                            0x003A
struct hciWriteCurrentIacLapItem {
    uint8_t iacLap[3];
} __packed;
struct hciWriteCurrentIacLap {
    uint8_t numCurrentIac;
    struct hciWriteCurrentIacLapItem items[];
} __packed;

#define HCI_CMD_Read_Page_Scan_Period_Mode                       0x003B

#define HCI_CMD_Write_Page_Scan_Period_Mode                      0x003C
struct hciWritePageScanPeriodMode {
    uint8_t mode;
} __packed;

#define HCI_CMD_Read_Page_Scan_Mode                              0x003D /* deprecated in BT 1.2 and later */

#define HCI_CMD_Write_Page_Scan_Mode                             0x003E /* deprecated in BT 1.2 and later */
struct hciWritePageScanMode {
    uint8_t pageScanMode; /* nonzero modes are optional */
} __packed;


/* ==== BT 1.2 ==== */

#define HCI_CMD_Set_AFH_Host_Channel_Classification              0x003F
struct hciSetAfhHostChannelClassification {
    uint8_t channels[10];
} __packed;

#define HCI_CMD_Read_Inquiry_Scan_Type                           0x0042

#define HCI_CMD_Write_Inquiry_Scan_Type                          0x0043
struct hciWriteInquiryScanType {
    uint8_t interlaced; /* optional */
} __packed;

#define HCI_CMD_Read _Inquiry_Mode                               0x0044

#define HCI_CMD_Write_Inquiry_Mode                               0x0045
struct hciWriteInquryMode {
    uint8_t inqMode; /* HCI_INQ_MODE_* */
} __packed;

#define HCI_CMD_Read_Page_Scan_Type                              0x0046

#define HCI_CMD_Write_Page_Scan_Type                             0x0047
struct hciWritePageScanType {
    uint8_t interlaced; /* optional */
} __packed;

#define HCI_CMD_Read_AFH_Channel_Assessment_Mode                 0x0048

#define HCI_CMD_Write_AFH_Channel_Assessment_Mode                0x0049
struct hciWriteAfhChannelAssessment {
    uint8_t channelAssessmentEnabled;
} __packed;


/* ==== BT 2.1 ==== */

#define HCI_CMD_Read_Extended_Inquiry_Reponse                    0x0051

#define HCI_CMD_Write_Extended_Inquiry_Response                  0x0052
struct hciWriteEIR {
    uint8_t useFec;
    uint8_t data[240];
} __packed;

#define HCI_CMD_Refresh_Encryption_Key                           0x0052
struct hciRefreshEncryptionKey {
    uint16_t conn;
} __packed;

#define HCI_CMD_Read_Simple_Pairing_Mode                         0x0055

#define HCI_CMD_Write_Simple_Pairing_Mode                        0x0056
struct hciWriteSimplePairingMore {
    uint8_t useSsp;
} __packed;

#define HCI_CMD_Read_Local_OOB_Data                              0x0057

#define HCI_CMD_Read_Inquiry_Response_Transmit_Power_Level       0x0058

#define HCI_CMD_Write_Inquiry_Transmit_Power_Level               0x0059
struct hciCmdWriteInquiryTransmitPowerLevel {
    uint8_t power;
} __packed;

#define HCI_CMD_Read_Default_Erroneous_Data_Reporting            0x005A

#define HCI_CMD_Write_Default_Erroneous_Data_Reporting           0x005B
struct hciWriteErroneousDataReporting {
    uint8_t reportingEnabled;
} __packed;

#define HCI_CMD_Enhanced_Flush                                   0x005F
struct hciEnhancedFlush {
    uint16_t conn;
    uint8_t which; /* 0 is the only value - flush auto-flushable packets only */
} __packed;

#define HCI_CMD_Send_Keypress_Notification                       0x0060
struct hciSendKeypressNotification {
    uint8_t mac[6];
    uint8_t notifType; /* HCI_SSP_KEY_ENTRY_* */
} __packed;


/* ==== BT 3.0 ==== */

#define HCI_CMD_Read_Logical_Link_Accept_Timeout                 0x0061

#define HCI_CMD_Write_Logical_Link_Accept_Timeout                0x0062
struct hciWriteLogicalLinkTimeout {
    uint16_t timeout; /* inunits of 0.625ms 1..0xB540. Required support 0x00A0..0xB540 */
} __packed;

#define HCI_CMD_Set_Event_Mask_Page_2                            0x0063
struct hciSetEventMaskPage2 {
    uint64_t evens; /* bitmask of HCI_EVENT_P2_* */
} __packed;

#define HCI_CMD_Read_Location_Data                               0x0064

#define HCI_CMD_Write_Location_Data                              0x0065
struct hciWriteLocationData {
    uint8_t regulatoryDomainKnown;
    uint16_t domain; /* ISO3166-1 code if known, else 0x5858 'XX' */
    uint8_t locationSuffix; /* HCI_LOCATION_DOMAIN_OPTION_* */
    uint8_t mainsPowered;
} __packed;

#define HCI_CMD_Read_Flow_Control_Mode                           0x0066

#define HCI_CMD_Write_Flow_Control_mode                          0x0067
struct hciWriteFlowcontrolMode {
    uint8_t blockBased; /* block based is for amp, packed-based is for BR/EDR */
} __packed;

#define HCI_CMD_Read_Best_Effort_Flush_Timeout                   0x0069
struct hciReadBestEffortFlushTimeout {
    uint16_t logicalLinkHandle;
} __packed;

#define HCI_CMD_Write_Best_Effort_Flush_Timeout                  0x006A
struct hciWriteBestEffortFlushTimeout {
    uint16_t logicalLinkHandle;
    uint32_t bestEffortFlushTimeout; /* in microseconds */
} __packed;

#define HCI_CMD_Short_Range_Mode                                 0x006B
struct hciShortRangeMode {
    uint8_t physicalLinkHandle;
    uint8_t shortRangeModeEnabled;
} __packed;

#define HCI_CMD_Read_LE_Host_Supported                           0x006C

#define HCI_CMD_Write_LE_Host_Supported                          0x006D
struct hciWriteLeHostSupported {
    uint8_t leSupportedHost;
    uint8_t simultaneousLeHost;
} __packed;


/* ==== BT 4.1 ==== */

#define HCI_CMD_Set_MWS_Channel_Parameters                       0x006E
struct hciSetMwsChannelParams {
    uint8_t mwsEnabled;
    uint16_t mwsChannelRxCenterFreq; /* in MHz */
    uint16_t mwsChannelTxCenterFreq; /* in MHz */
    uint16_t mwsChannelRxBandwidth; /* in MHz */
    uint16_t mwsChannelTxBandwidth; /* in MHz */
    uint8_t mwsChannelType;
} __packed;

#define HCI_CMD_Set_External_Frame_Configuration                 0x006F
struct hciSetExternalFrameConfigItem {
    uint16_t periodDuration; /* in microseconds */
    uint8_t periodType; /* HCI_PERIOD_TYPE_* */
} __packed;
struct hciSetExternalFrameConfig {
    uint16_t extFrameDuration; /* in microseonds */
    uint16_t extFrameSyncAssertOffset; /* inmicroseonds */
    uint16_t extFrameSyncAssertJitter; /* inmicroseonds */
    uint8_t extNumPeriods; /* 1 .. 32 */
    struct hciSetExternalFrameConfigItem items[];
} __packed;

#define HCI_CMD_Set_MWS_Signalling                               0x0070
struct hciSetMwsSignalling {
    uint16_t mwsRxAssertOffset; /* all of these are in microseconds */
    uint16_t mwsRxAssertJitter;
    uint16_t mwsRxDeassertOffset;
    uint16_t mwsRxDeassertJitter;
    uint16_t mwsTxAssertOffset;
    uint16_t mwsTxAssertJitter;
    uint16_t mwsTxDeassertOffset;
    uint16_t mwsTxDeassertJitter;
    uint16_t mwsPatternAssertOffset;
    uint16_t mwsPatternAssertJitter;
    uint16_t mwsInactivityDurationAssertOffset;
    uint16_t mwsInactivityDurationAssertJitter;
    uint16_t mwsScanFrequencyAssertOffset;
    uint16_t mwsScanFrequencyAssertJitter;
    uint16_t mwsPriorityAssertOffsetRequest;
} __packed;

#define HCI_CMD_Set_MWS_Transport_Layer                          0x0071
struct hciSetMwsTransportLayer {
    uint8_t transportLayer;
    uint32_t toMwsBaudRate; /* in byte/sec */
    uint32_t fromMwsBaudRate; /* in byte/sec */
} __packed;

#define HCI_CMD_Set_MWS_Scan_Frequency_Table                     0x0072
struct hciSetMwsScanFrequencyTableItem {
    uint16_t scanFreqLow; /*in MHz */
    uint16_t scanFreqHigh; /*in MHz */
} __packed;
struct hciSetMwsScanFrequencyTable {
    uint8_t n;
    struct hciSetMwsScanFrequencyTableItem items[];
} __packed;

#define HCI_CMD_Set_MWS_PATTERN_Configuration                    0x0073
struct hciSetMwsPatternConfigItem {
    uint16_t intervalDuration; /* in microseconds */
    uint8_t intervalType; /* HCI_MWS_INTERVAL_TYPE_* */
} __packed;
struct hciSetMwsPatternConfig {
    uint8_t mwsPatternIndex; /* 0 .. 2 */
    uint8_t mwsPatternNumIntervals;
    struct hciSetMwsPatternConfigItem items[];
} __packed;

#define HCI_CMD_Set_Reserved_LT_ADDR                             0x0074
struct hciSetReservedLtAddr {
    uint8_t ltAddr;
} __packed;

#define HCI_CMD_Delete_Reserved_LT_ADDR                          0x0075
struct hciDeleteReservedLtAddr {
    uint8_t ltAddr;
} __packed;

#define HCI_CMD_Set_Connectionless_Slave_Broadcast_Data          0x0076
struct hciSetConnlessSlaveBroadcastData {
    uint8_t ltAddr;
    uint8_t fragment; /* HCI_CONNLESS_FRAG_TYPE_* */
    uint8_t dataLen;
    uint8_t data[];
} __packed;

#define HCI_CMD_Read_Synchronisation_Train_Parameters            0x0077

#define HCI_CMD_Write_Synchronisation_Train_Parameters           0x0078
struct hciWriteSyncTrainParams {
    uint16_t intMin;
    uint16_t intMax;
    uint32_t syncTrainTimeout;
    uint8_t serviceData;
} __packed;

#define HCI_CMD_Read_Secure_Connections_Host_Support             0x0079

#define HCI_CMD_Write_Secure_Connections_Host_Support            0x007A
struct hciWriteSecureConnectionsHostSupport {
    uint8_t secureConnectionsSupported;
} __packed;

#define HCI_CMD_Read_Authenticated_Payload_Timeout               0x007B
struct hciReadAuthedPayloadTimeout {
    uint16_t conn;
} __packed;

#define HCI_CMD_Write_Authenticated_Payload_Timeout              0x007C
struct hciWriteAuthedPayloadTimeout {
    uint16_t conn;
    uint16_t timeout; /* in units of 10ms, 1 .. 0xffff */
} __packed;

#define HCI_CMD_Read_Local_OOB_Extended_Data                     0x007D

#define HCI_CMD_Read_Extended_Page_Timeout                       0x007E

#define HCI_CMD_Write_Extended_Page_Timeout                      0x007F
struct hciWriteExtendedPageTimeout {
    uint16_t timeout; /* in units of 0.625ms 0..0xffff */
} __packed;

#define HCI_CMD_Read_Extended_Inquiry_Length                     0x0080

#define HCI_CMD_Write_Extended_Inquiry_Length                    0x0081
struct hciWriteExtendedInquiryLength {
    uint16_t timeout; /* in units of 0.625ms 0..0xffff */
} __packed;





#define HCI_OGF_Informational                                    4


/* ==== BT 1.1 ==== */

#define HCI_CMD_Read_Local_Version_Information                   0x0001

#define HCI_CMD_Read_Local_Supported_Commands                    0x0002

#define HCI_CMD_Read_Local_Supported_Features                    0x0003

#define HCI_CMD_Read_Local_Extended _Features                    0x0004
struct hciReadLocalExtendedFeatures {
    uint8_t page;
} __packed;

#define HCI_CMD_Read_Buffer_Size                                 0x0005

#define HCI_CMD_Read_BD_ADDR                                     0x0009


/* ==== BT 3.0 ==== */

#define HCI_CMD_Read_Data_Block_Size                             0x000A



/* ==== BT 4.1 ==== */

#define HCI_CMD_Read_Local_Suported_Codecs                       0x000B





#define HCI_OGF_Status                                           5


/* == BT 1.1 == */

#define HCI_CMD_Read_Failed_Contact_Counter                      0x0001
struct hciReadFailedContactCounter {
    uint16_t conn;
} __packed;

#define HCI_CMD_Reset_Failed_Contact_Counter                     0x0002
struct hciResetFailedContactCounter {
    uint16_t conn;
} __packed;

#define HCI_CMD_Read_Link_Quality                                0x0003
struct hciReadLinkQuality {
    uint16_t conn;
} __packed;

#define HCI_CMD_Read_RSSI                                        0x0005
struct hciReadRssi {
    uint16_t conn;
} __packed;


/* ==== BT 1.2 ==== */

#define HCI_CMD_Read_AFH_Channel_Map                             0x0006
struct hciReadAfhChannelMap {
    uint16_t conn;
} __packed;


#define HCI_CMD_Read_Clock                                       0x0007
struct hciReadClock {
    uint16_t conn;
    uint8_t readRemote; /* else reads local and ignores conn */
} __packed;


/* ==== BT 3.0 ==== */

#define HCI_CMD_Read_Encryption_Key_Size                         0x0008
struct hciReadEncrKeySize {
    uint16_t conn;
} __packed;

#define HCI_CMD_Read_Local_AMP_Info                              0x0009

#define HCI_CMD_Read_Local_AMP_ASSOC                             0x000A
struct hciReadLocalAmpAssoc {
    uint8_t physiclaLinkHandle;
    uint16_t lengthSoFar;
    uint16_t ampAssocLen;
} __packed;

#define HCI_CMD_Write_Remote_AMP_ASSOC                           0x000B
struct hciWriteRemoteAmpAssoc {
    uint8_t physiclaLinkHandle;
    uint16_t lengthSoFar;
    uint16_t remaningLength;
    uint8_t fragment[]; /* 248 bytes for all but last one */
} __packed;

/* ==== BT 4.1 ==== */

#define HCI_CMD_Get_MWS_Transport_Layer_Configuration            0x000C

#define HCI_CMD_Set_Triggered_Clock_Capture                      0x000D
struct hciSetTriggeredClockCapture {
    uint16_t conn;
    uint8_t enable;
    uint8_t piconetClock; /* else local clock & "conn" is ignored */
    uint8_t lpoAllowed; /* can sleep? */
    uint8_t numClockCapturesToFilter;
} __packed;





#define HCI_OGF_LE                                               8


/* ==== BT 4.0 ==== */

#define HCI_CMD_LE_Set_Event_Mask                                0x0001
struct hciLeSetEventMask {
    uint64_t events; /* bitmask of HCI_LE_EVENT_* */
} __packed;

#define HCI_CMD_LE_Read_Buffer_Size                              0x0002

#define HCI_CMD_LE_Read_Local_Supported_Features                 0x0003

#define HCI_CMD_LE_Set_Random_Address                            0x0005
struct hciLeSetRandomAddress{
    uint8_t mac[6];
} __packed;

#define HCI_CMD_LE_Set_Adv_Params                                0x0006
struct hciLeSetAdvParams {
    uint16_t advIntervalMin;
    uint16_t advIntervalMax;
    uint8_t advType;
    uint8_t useRandomAddress;
    uint8_t directRandomAddress;
    uint8_t directAddr[6];
    uint8_t advChannelMap;
    uint8_t advFilterPolicy;
} __packed;

#define HCI_CMD_LE_Read_Adv_Channel_TX_Power                     0x0007

#define HCI_CMD_LE_Set_Advertising_Data                          0x0008
struct hciLeSetAdvData {
    uint8_t advDataLen;
    uint8_t advData[31];
} __packed;

#define HCI_CMD_LE_Set_Scan_Response_Data                        0x0009
struct hciSetScanREsponseData {
    uint8_t scanRspDataLen;
    uint8_t scanRspData[31];
} __packed;

#define HCI_CMD_LE_Set_Advertise_Enable                          0x000A
struct hciLeSetAdvEnable {
    uint8_t advOn;
} __packed;

#define HCI_CMD_LE_Set_Scan_Parameters                           0x000B
struct hciLeSetScanParams {
    uint8_t activeScan;
    uint16_t scanInterval; /* in units of 0.625ms, 4..0x4000 */
    uint16_t scanWindow; /* in units of 0.625ms, 4..0x4000 */
    uint8_t useOwnRandomAddr;
    uint8_t onlyWhitelist;
} __packed;

#define HCI_CMD_LE_Set_Scan_Enable                               0x000C
struct hciLeSetScanEnable {
    uint8_t scanOn;
    uint8_t filterDuplicates;
} __packed;

#define HCI_CMD_LE_Create_Connection                             0x000D
struct hciLeCreateConnection {
    uint16_t scanInterval; /* in units of 0.625ms, 4..0x4000 */
    uint16_t scanWindow; /* in units of 0.625ms, 4..0x4000 */
    uint8_t connectToAnyWhitelistedDevice; /* if so, ignore next 2 params */
    uint8_t peerRandomAddr;
    uint8_t peerMac[6];
    uint8_t useOwnRandomAddr;
    uint16_t connIntervalMin; /* in units of 1.25ms, 6..0x0C80 */
    uint16_t connIntervalMax; /* in units of 1.25ms, 6..0x0C80 */
    uint16_t connLatency; /* 0..0x1F4 */
    uint16_t supervisionTimeout; /* in units of 10ms, 0xA...0x0C80 */
    uint16_t minConnLen; /* minimum conn len needed in units of 0.625ms 0..0xfff */
    uint16_t maxConnLen; /* minimum conn len needed in units of 0.625ms 0..0xfff */
} __packed;

#define HCI_CMD_LE_Create_Connection_Cancel                      0x000E

#define HCI_CMD_LE_Read_White_List_Size                          0x000F

#define HCI_CMD_LE_Clear_White_List                              0x0010

#define HCI_CMD_LE_Add_Device_To_White_List                      0x0011
struct hciLeAddDeviceToWhiteList {
    uint8_t randomAddr;
    uint8_t mac[6];
} __packed;

#define HCI_CMD_LE_Remove_Device_From_White_List                 0x0012
struct hciLeRemoveDeviceFromWhiteList {
    uint8_t randomAddr;
    uint8_t mac[6];
} __packed;

#define HCI_CMD_LE_Connection_Update                             0x0013
struct hciLeConnectionUpdate {
    uint16_t conn;
    uint16_t connIntervalMin; /* in units of 1.25ms, 6..0x0C80 */
    uint16_t connIntervalMax; /* in units of 1.25ms, 6..0x0C80 */
    uint16_t connLatency; /* 0..0x1F4 */
    uint16_t supervisionTimeout; /* in units of 10ms, 0xA...0x0C80 */
    uint16_t minConnLen; /* minimum conn len needed in units of 0.625ms 0..0xfff */
    uint16_t maxConnLen; /* minimum conn len needed in units of 0.625ms 0..0xfff */
} __packed;

#define HCI_CMD_LE_Set_Host_Channel_Classification               0x0014
struct hciLeSetHostChannelClassification {
    uint8_t chMap[5];
} __packed;

#define HCI_CMD_LE_Read_Channel_Map                              0x0015
struct hciLeReadChannelMap {
    uint16_t conn;
} __packed;

#define HCI_CMD_LE_Read_Remote_Used_Features                     0x0016
struct hciLeReadRemoteUsedFeatures {
    uint16_t conn;
} __packed;

#define HCI_CMD_LE_Encrypt                                       0x0017
struct hciLeEncrypt {
    uint8_t key[16];
    uint8_t plaintext[16];
} __packed;

#define HCI_CMD_LE_Rand                                          0x0018

#define HCI_CMD_LE_Start_Encryption                              0x0019
struct hciLeStartEncryption {
    uint16_t conn;
    uint64_t rand;
    uint16_t diversifier;
    uint8_t LTK[16];
} __packed;

#define HCI_CMD_LE_LTK_Request_Reply                             0x001A
struct hciLeLtkRequestReply {
    uint16_t conn;
    uint8_t LTK[16];
} __packed;

#define HCI_CMD_LE_LTK_Request_Negative_Reply                    0x001B
struct hciLeLtkRequestNEgativeReply {
    uint16_t conn;
} __packed;

#define HCI_CMD_LE_Read_Supported_States                         0x001C

#define HCI_CMD_LE_Receiver_Test                                 0x001D
struct hciLeReceiverTest {
    uint8_t radioChannelNum; /* 2402 + radioChannelNum * 2 MHz */
} __packed;

#define HCI_CMD_LE_Transmitter_Test                              0x001E
struct hciLeTransmitterTest {
    uint8_t radioChannelNum; /* 2402 + radioChannelNum * 2 MHz */
    uint8_t lengthOfTestData;
    uint8_t testPacketDataType;
} __packed;

#define HCI_CMD_LE_Test_End                                      0x001F


/* ==== BT 4.1 ==== */

#define HCI_CMD_Remote_Connection_Param_Request_Reply            0x0020
struct hciRemoteConnParamRequestReply {
    uint16_t conn;
    uint16_t connIntervalMin; /* in units of 1.25ms, 6..0x0C80 */
    uint16_t connIntervalMax; /* in units of 1.25ms, 6..0x0C80 */
    uint16_t connLatency; /* 0..0x1F4 */
    uint16_t supervisionTimeout; /* in units of 10ms, 0xA...0x0C80 */
    uint16_t minConnLen; /* minimum conn len needed in units of 0.625ms 0..0xfff */
    uint16_t maxConnLen; /* minimum conn len needed in units of 0.625ms 0..0xfff */
} __packed;

#define HCI_CMD_Remote_Connection_Param_Request_Negative_Reply   0x0020
struct hciRemoteConnParamRequestNegativeReply {
    uint16_t conn;
    uint8_t reason;
} __packed;




/* EVENTS */


/* ==== BT 1.1 ==== */

#define HCI_EVT_Inquiry_Complete                                 0x01
struct hciEvtInquiryComplete {
    uint8_t status;
} __packed;

#define HCI_EVT_Inquiry_Result                                   0x02
struct hciEvtInquiryResultItem {
    uint8_t mac[6];
    uint8_t PSRM;
    uint8_t PSPM;
    uint8_t PSM; /* obsoleted in BT 1.2+ */
    uint8_t deviceClass[3];
    uint16_t clockOffset;
} __packed;
struct hciEvtInquiryResult {
    uint8_t numResponses;
    struct hciEvtInquiryResultItem items[];
} __packed;

#define HCI_EVT_Connection_Complete                              0x03
struct hciEvtConnComplete {
    uint8_t status;
    uint16_t conn;
    uint8_t mac[6];
    uint8_t isAclLink;
    uint8_t encrypted;
} __packed;

#define HCI_EVT_Connection_Request                               0x04
struct hciEvtConnRequest {
    uint8_t mac[6];
    uint8_t deviceClass[3];
    uint8_t isAclLink;
} __packed;

#define HCI_EVT_Disconnection_Complete                           0x05
struct hciEvtDiscComplete {
    uint8_t status;
    uint16_t conn;
    uint8_t reason;
} __packed;

#define HCI_EVT_Authentication_Complete                          0x06
struct hciEvtAuthComplete {
    uint8_t status;
    uint16_t handle;
} __packed;

#define HCI_EVT_Remote_Name_Request_Complete                     0x07
struct hciEvtRemoteNameReqComplete {
    uint8_t status;
    uint8_t mac[6];
    char name[248];
} __packed;

#define HCI_EVT_Encryption_Change                                0x08
struct hciEvtEncrChange {
    uint8_t status;
    uint16_t handle;
    uint8_t encrOn;
} __packed;

#define HCI_EVT_Change_Connection_Link_Key_Complete              0x09
struct hciEvtChangeConnLinkKeyComplete {
    uint8_t status;
    uint16_t handle;
} __packed;

#define HCI_EVT_Master_Link_Key_Complete                         0x0A
struct hciEvtMasterLinkKeyComplete {
    uint8_t status;
    uint16_t conn;
    uint8_t usingTempKey; /* else using semi-permanent key */
} __packed;

#define HCI_EVT_Read_Remote_Supported_Features_Complete          0x0B
struct hciEvtReadRemoteSupportedFeaturesComplete {
    uint8_t status;
    uint16_t conn;
    uint64_t lmpFeatures; /* bitmask of HCI_LMP_FTR_* */
} __packed;

#define HCI_EVT_Read_Remote_Version_Complete                     0x0C
struct hciEvtReadRemoteVersionComplete {
    uint8_t status;
    uint16_t conn;
    uint8_t lmpVersion; /* HCI_LMP_VERSION_* */
    uint16_t manufName;
    uint16_t lmpSubversion;
} __packed;

#define HCI_EVT_QOS_Setup_Complete                               0x0D
struct hciEvtQosSetupComplete {
    uint8_t status;
    uint16_t conn;
    uint8_t flags;
    uint8_t serviceType;
    uint32_t tokenRate;
    uint32_t peakBandwidth;
    uint32_t latency;
    uint32_t delayVariation;
} __packed;

#define HCI_EVT_Command_Complete                                 0x0E
struct hciEvtCmdComplete {
    uint8_t numCmdCredits;
    uint16_t opcode;
} __packed;

#define HCI_EVT_Command_Status                                   0x0F
struct hciEvtCmdStatus {
    uint8_t status;
    uint8_t numCmdCredits;
    uint16_t opcode;
} __packed;

#define HCI_EVT_Hardware_Error                                   0x10
struct hciEvtHwError {
    uint8_t errCode;
} __packed;

#define HCI_EVT_Flush_Occured                                    0x11
struct hciEvtFlushOccured {
    uint16_t conn;
} __packed;

#define HCI_EVT_Role_Change                                      0x12
struct hciEvtRoleChange {
    uint8_t status;
    uint8_t mac[6];
    uint8_t amSlave;
} __packed;

#define HCI_EVT_Number_Of_Completed_Packets                      0x13
struct hciEvtNumCompletedPacketsItem {
    uint8_t conn;
    uint16_t numPackets;
} __packed;
struct hciEvtNumCompletedPackets {
    uint8_t numHandles;
    struct hciEvtNumCompletedPacketsItem items[];
} __packed;

#define HCI_EVT_Mode_Change                                      0x14
struct hciEvtModeChange {
    uint8_t status;
    uint16_t conn;
    uint8_t mode; /* HCI_CUR_MODE_* */
    uint16_t interval; /* in units of 0.625ms 0..0xffff */
} __packed;

#define HCI_EVT_Return_Link_Keys                                 0x15
struct hciEvtReturnLinkKeysItem {
    uint8_t mac[6];
    uint8_t key[16];
} __packed;
struct hciEvtReturnLinkKeys {
    uint8_t numKeys;
    struct hciEvtReturnLinkKeysItem items[];
} __packed;

#define HCI_EVT_PIN_Code_Request                                 0x16
struct hciEvtPinCodeReq {
    uint8_t mac[6];
} __packed;

#define HCI_EVT_Link_Key_Request                                 0x17
struct hciEvtLinkKeyReq {
    uint8_t mac[6];
} __packed;

#define HCI_EVT_Link_Key_Notification                            0x18
struct hciEvtLinkKeyNotif {
    uint8_t mac[6];
    uint16_t key[16];
    uint8_t keyType; /* HCI_KEY_TYPE_ */
} __packed;

#define HCI_EVT_Loopback_Command                                 0x19
/* data is the sent command, up to 252 bytes of it */

#define HCI_EVT_Data_Buffer_Overflow                             0x1A
struct hciEvtDataBufferOverflow {
    uint8_t aclLink;
} __packed;

#define HCI_EVT_Max_Slots_Change                                 0x1B
struct hciEvtMaxSlotsChange {
    uint16_t conn;
    uint8_t lmpMaxSlots;
} __packed;

#define HCI_EVT_Read_Clock_Offset_Complete                       0x1C
struct hciEvtReadClockOffsetComplete {
    uint8_t status;
    uint16_t conn;
    uint16_t clockOffset;
} __packed;

#define HCI_EVT_Connection_Packet_Type_Changed                   0x1D
struct hciEvtConnPacketTypeChanged {
    uint8_t status;
    uint16_t conn;
    uint16_t packetsAllowed; /* HCI_PKT_TYP_* */
} __packed;

#define HCI_EVT_QoS_Violation                                    0x1E
struct hciEvtQosViolation {
    uint16_t conn;
} __packed;

#define HCI_EVT_Page_Scan_Mode_Change                            0x1F /* deprecated in BT 1.2+ */
struct hciEvtPsmChange {
    uint8_t mac[6];
    uint8_t PSM;
} __packed;

#define HCI_EVT_Page_Scan_Repetition_Mode_Change                 0x20
struct hciEvtPrsmChange {
    uint8_t mac[6];
    uint8_t PSRM;
} __packed;


/* ==== BT 1.2 ==== */

#define HCI_EVT_Flow_Specification_Complete                      0x21
struct hciEvtFlowSpecComplete {
    uint8_t status;
    uint16_t conn;
    uint8_t flags;
    uint8_t flowDirection;
    uint8_t serviceType;
    uint32_t tokenRate;
    uint32_t peakBandwidth;
    uint32_t latency;
} __packed;

#define HCI_EVT_Inquiry_Result_With_RSSI                         0x22
struct hciEvtInquiryResultWithRssiItem {
    uint8_t mac[6];
    uint8_t PSRM;
    uint8_t PSPM;
    uint8_t deviceClass[3];
    uint16_t clockOffset;
    uint8_t RSSI; /* actually a int8_t */
} __packed;
struct hciEvtInquiryResultWithRssi {
    uint8_t numResponses;
    struct hciEvtInquiryResultWithRssiItem items[];
} __packed;

#define HCI_EVT_Read_Remote_Extended_Features_Complete           0x23
struct hciEvtReadRemoteExtFeturesComplete {
    uint8_t status;
    uint16_t handle;
    uint8_t pageNum;
    uint8_t maxPageNum;
    uint64_t extLmpFeatures; /* HCI_LMP_EXT_FTR_P* & HCI_LMP_FTR_* */
} __packed;

#define HCI_EVT_Synchronous_Connection_Complete                  0x2C
struct hciEvtSyncConnComplete {
    uint8_t status;
    uint16_t conn;
    uint8_t mac[6];
    uint8_t linkType; /* HCI_SCO_LINK_TYPE_* */
    uint8_t interval;
    uint8_t retrWindow;
    uint16_t rxPacketLen;
    uint16_t txPacketLen;
    uint8_t airMode; /* HCI_SCO_AIR_MODE_* */
} __packed;

#define HCI_EVT_Synchronous_Connection_Changed                   0x2D
struct hciEvtSyncConnChanged {
    uint8_t status;
    uint16_t conn;
    uint8_t interval;
    uint8_t retrWindow;
    uint16_t rxPacketLen;
    uint16_t txPacketLen;
} __packed;


/* ==== BT 2.1 ==== */

#define HCI_EVT_Sniff_Subrating                                  0x2E
struct hciEvtSniffSubrating {
    uint8_t status;
    uint16_t conn;
    uint16_t maxTxLatency;
    uint16_t maxRxLatency;
    uint16_t minRemoteTimeout;
    uint16_t minLocalTimeout;
} __packed;

#define HCI_EVT_Extended_Inquiry_Result                          0x2F
struct hciEvtExtendedInquiryResult {
    uint8_t numResponces; /* must be 1 */
    uint8_t mac[6];
    uint8_t PSRM;
    uint8_t reserved;
    uint8_t deviceClass[3];
    uint16_t clockOffset;
    uint8_t RSSI; /* actually a int8_t */
    uint8_t EIR[240];
} __packed;

#define HCI_EVT_Encryption_Key_Refresh_Complete                  0x30
struct hciEvtEncrKeyRefreshComplete {
    uint8_t status;
    uint16_t conn;
} __packed;

#define HCI_EVT_IO_Capability_Request                            0x31
struct hciEvtIoCapRequest {
    uint8_t mac[6];
} __packed;

#define HCI_EVT_IO_Capability_Response                           0x32
struct hciEvtIoCapResponse {
    uint8_t mac[6];
    uint8_t ioCapability; /* HCI_DISPLAY_CAP_* */
    uint8_t oobDataPresent;
    uint8_t authReqments; /* HCI_AUTH_REQMENT_ */
} __packed;

#define HCI_EVT_User_Confirmation_Request                        0x33
struct hciEvtUserConfRequest {
    uint8_t mac[6];
    uint32_t numericValue;
} __packed;

#define HCI_EVT_User_Passkey_Request                             0x34
struct hciEvtUserPasskeyRequest {
    uint8_t mac[6];
} __packed;

#define HCI_EVT_Remote_OOB Data_Request                          0x35
struct hciEvtRemoteOobRequest {
    uint8_t mac[6];
} __packed;

#define HCI_EVT_Simple_Pairing_Complete                          0x36
struct hciEvtSimplePaitingComplete {
    uint8_t status;
    uint8_t mac[6];
} __packed;

#define HCI_EVT_Link_Supervision_Timeout_Changed                 0x38
struct hciEvtLinkSupervisionTimeoutChanged {
    uint16_t conn;
    uint16_t timeout; /* in units of 0.625 ms 1..0xffff */
} __packed;

#define HCI_EVT_Enhanced_Flush_Complete                          0x39
struct hciEvtEnahncedFlushComplete {
    uint16_t conn;
} __packed;

#define HCI_EVT_User_Passkey_Notification                        0x3B
struct hciEvtUserPasskeyNotif {
    uint8_t mac[6];
    uint32_t passkey;
} __packed;

#define HCI_EVT_Keypress_Notification                            0x3C
struct hciEvtKeypressNotification {
    uint8_t mac[6];
    uint8_t notifType; /* HCI_SSP_KEY_ENTRY_* */
} __packed;

#define HCI_EVT_Remote_Host_Supported_Features_Notification      0x3D
struct hciEvtRemoteHostSupportedFeatures {
    uint8_t mac[6];
    uint64_t hostSupportedFeatures; /* HCI_LMP_FTR_* */
} __packed;


/* ==== BT 3.0 ==== */

#define HCI_EVT_Physical_Link_Complete                           0x40
struct hciEvtPhysLinkComplete {
    uint8_t status;
    uint8_t physLinkHandle;
} __packed;

#define HIC_EVT_Channel_Selected                                 0x41
struct hciEvtChannelSelected {
    uint8_t physLinkHandle;
} __packed;

#define HCI_EVT_Disconnection_Physical_Link_Complete             0x42
struct hciEvtDiscPhysLinkComplete {
    uint8_t status;
    uint8_t physLinkHandle;
    uint8_t reason;
} __packed;

#define HCI_EVT_Physical_Link_Loss_Early_Warning                 0x43
struct hciEvtDiscPhysLinkLossEralyWarning {
    uint8_t physLinkHandle;
    uint8_t lossReason;
} __packed;

#define HCI_EVT_Physical_Link_Recovery                           0x44
struct hciEvtDiscPhysLinkRecovery {
    uint8_t physLinkHandle;
} __packed;

#define HCI_EVT_Logical_Link_Complete                            0x45
struct hciEvtLogicalLinkComplete {
    uint8_t status;
    uint16_t logicalLinkHandle;
    uint8_t physLinkHandle;
    uint8_t txFlowSpecID;
} __packed;

#define HCI_EVT_Disconnection_Logical_Link_Complete              0x46
struct hciEvtDiscLogicalLinkComplete {
    uint8_t status;
    uint16_t logicalLinkHandle;
    uint8_t reason;
} __packed;

#define HCI_EVT_Flow_Spec_Modify_Complete                        0x47
struct hciEvtFlowSpecModifyComplete {
    uint8_t status;
    uint16_t conn;
} __packed;

#define HCI_EVT_Number_Of_Completed_Data_Blocks                  0x48
struct hciEvtNumCompletedDataBlocksItem {
    uint16_t conn;
    uint16_t numPackets;
} __packed;
struct hciEvtNumCompletedDataBlocks {
    uint16_t totalNumBlocks;
    uint8_t numberOfHandles;
    struct hciEvtNumCompletedDataBlocksItem items[];
} __packed;

#define HCI_EVT_AMP_Start_Test                                   0x49
struct hciEvtAmpStartTest {
    uint8_t status;
    uint8_t scenario;
} __packed;

#define HCI_EVT_AMP_Test_End                                     0x4A
struct hciEvtAmpTestEnd {
    uint8_t status;
    uint8_t scenario;
} __packed;

#define HCI_EVT_AMP_Receiver_Report                              0x4B
struct hciEvtampReceiverReport {
    uint8_t controllerType;
    uint8_t reason;
    uint32_t eventType;
    uint16_t numberOfFrames;
    uint16_t numberOfErrorFrames;
    uint32_t numberOfBits;
    uint32_t numberOfErrorBits;
} __packed;

#define HCI_EVT_Short_Range_Mode_Change_Complete                 0x4C
struct hciEvtshortRangeModeChangeComplete {
    uint8_t status;
    uint8_t physLinkHandle;
    uint8_t shortRangeModeOn;
} __packed;

#define HCI_EVT_AMP_Status_Change                                0x4D
struct hciEvtAmpStatusChange {
    uint8_t status;
    uint8_t ampStatus;
} __packed;


/* ==== BT 4.0 ==== */

#define HCI_EVT_LE_Meta                                          0x3E
struct hciEvtLeMeta {
    uint8_t subevent;
} __packed;

#define HCI_EVTLE_Connection_Complete                            0x01
struct hciEvtLeConnectionComplete {
    uint8_t status;
    uint16_t conn;
    uint8_t amSlave;
    uint8_t peerAddrRandom;
    uint8_t peerMac[6];
    uint16_t connInterval; /* in units of 1.25 ms 6..0x0C80 */
    uint16_t connLatency; /* 0..0x01f3 */
    uint16_t supervisionTimeout; /* inunit sof 10ms, 0xA..0x0C80 */
    uint8_t masterClockAccuracy; /* HCI_MCA_* */
} __packed;

#define HCI_EVTLE_Advertising_Report                             0x02
struct hciEvtLeAdvReportItem {
    uint8_t advType; /* HCI_ADV_TYPE_* */
    uint8_t randomAddr;
    uint8_t mac[6];
    uint8_t dataLen;
    uint8_t data[];
/*  int8_t RSSI <-- this cannot be here due to variable data len, but in reality it is there */
} __packed;
struct hciEvtLeAdvReport {
    uint8_t numReports;
    /* struct hciEvtLeAdvReportItem items[]; <- this cannot be here since data length is variable */
} __packed;

#define HCI_EVTLE_Connection_Update_Complete                     0x03
struct hciEvtLeConnectionUpdateComplete {
    uint8_t status;
    uint16_t conn;
    uint16_t connInterval; /* in units of 1.25 ms 6..0x0C80 */
    uint16_t connLatency; /* 0..0x01f3 */
    uint16_t supervisionTimeout; /* inunit sof 10ms, 0xA..0x0C80 */
} __packed;

#define HCI_EVTLE_Read_Remote_Used_Features_Complete             0x04
struct hciEvtLeReadRemoteFeaturesComplete {
    uint8_t status;
    uint16_t conn;
    uint64_t leFeatures; /* bitmask of HCI_LE_FTR_* */
} __packed;

#define HCI_EVTLE_LTK_Request                                    0x05
struct hciEvtLeLtkRequest {
    uint16_t conn;
    uint64_t randomNum;
    uint16_t diversifier;
} __packed;


/* ==== BT 4.1 ==== */

#define HCI_EVTLE_Read_Remote_Connection_Parameter_Request       0x06
struct hciEvtLeReadRemoteConnParamRequest {
    uint16_t conn;
    uint16_t connIntervalMin; /* in units of 1.25 ms 6..0x0C80 */
    uint16_t connIntervalMax; /* in units of 1.25 ms 6..0x0C80 */
    uint16_t connLatency; /* 0..0x01f3 */
    uint16_t supervisionTimeout; /* inunit sof 10ms, 0xA..0x0C80 */
} __packed;

#define HCI_EVT_Triggered_Clock_Capture                          0x4E
struct hciEvtTriggeredClockCapture {
    uint16_t conn;
    uint8_t piconetClock;
    uint32_t clock;
    uint16_t slotOffset;
} __packed;

#define HCI_EVT_Synchronization_Train_Complete                   0x4F
struct hciEvtSyncTrainComplete {
    uint8_t status;
} __packed;

#define HCI_EVT_Synchronization_Train_Received                   0x50
struct hciEvtSyncTrainReceived {
    uint8_t status;
    uint8_t mac[6];
    uint32_t offset;
    uint8_t afhChannelMap[10];
    uint8_t ltAddr;
    uint32_t nextBroadcastInstant;
    uint16_t connectionlessSlaveBroadcastInterval;
    uint8_t serviceData;
} __packed;

#define HCI_EVT_Connectionless_Slave_Broadcast_Receive           0x51
struct hciEvtConnectionlessSlaveBroadcastReceive {
    uint8_t mac[6];
    uint8_t ltAddr;
    uint32_t clk;
    uint32_t offset;
    uint8_t rxFailed;
    uint8_t fragment; /* HCI_CONNLESS_FRAG_TYPE_* */
    uint8_t dataLen;
    /* data */
} __packed;

#define HCI_EVT_Connectionless_Slave_Broadcast_Timeout           0x52
struct hciEvtConnectionlessSlaveBroadcastTimeout {
    uint8_t mac[6];
    uint8_t ltAddr;
} __packed;

#define HCI_EVT_Truncated_Page_Complete                          0x53
struct hciEvtTruncatedPageComplete {
    uint8_t status;
    uint8_t mac[6];
} __packed;

#define HCI_EVT_Slave_Page_Response_Timeout                      0x54

#define HCI_EVT_Connless_Slave_Broadcast_Channel_Map_Change      0x55
struct hciEvtConnlessSlaveBroadcastChannelMapChange {
    uint8_t map[10];
} __packed;

#define HCI_EVT_Inquiry_Response_Notification                    0x56
struct hciEvtInquiryResponseNotif {
    uint8_t lap[3];
    uint8_t RSSI; /* actually an int8_t */
} __packed;

#define HCI_EVT_AuthenticateD_Payload_Timeout_Expired            0x57
struct hciEvtAuthedPayloadTimeoutExpired {
    uint16_t conn;
} __packed;


#define HCI_ERR_Unknown_HCI_Command                                           0x01
#define HCI_ERR_No_Connection                                                 0x02
#define HCI_ERR_Hardware_Failure                                              0x03
#define HCI_ERR_Page_Timeout                                                  0x04
#define HCI_ERR_Authentication_Failure                                        0x05
#define HCI_ERR_Key_Missing                                                   0x06
#define HCI_ERR_Memory_Full                                                   0x07
#define HCI_ERR_Connection_Timeout                                            0x08
#define HCI_ERR_Max_Number_Of_Connections                                     0x09
#define HCI_ERR_Max_Number_Of_SCO_Connections_To_A_Device                     0x0A
#define HCI_ERR_ACL_Connection_Already_Exists                                 0x0B
#define HCI_ERR_Command_Disallowed                                            0x0C
#define HCI_ERR_Host_Rejected_Due_To_Limited_Resources                        0x0D
#define HCI_ERR_Host_Rejected_Due_To_Security_Reasons                         0x0E
#define HCI_ERR_Host_Rejected_Due_To_A_Remote_Device_Only_A_Personal_Device   0x0F
#define HCI_ERR_Host_Timeout                                                  0x10
#define HCI_ERR_Unsupported_Feature_Or_Parameter_Value                        0x11
#define HCI_ERR_Invalid_HCI_Command_Parameters                                0x12
#define HCI_ERR_Other_End_Terminated_Connection_User_Ended_Connection         0x13
#define HCI_ERR_Other_End_Terminated_Connection_Low_Resources                 0x14
#define HCI_ERR_Other_End_Terminated_Connection_About_To_Power_Off            0x15
#define HCI_ERR_Connection_Terminated_By_Local_Host                           0x16
#define HCI_ERR_Repeated_Attempts                                             0x17
#define HCI_ERR_Pairing_Not_Allowed                                           0x18
#define HCI_ERR_Unknown_LMP_PDU                                               0x19
#define HCI_ERR_Unsupported_Remote_Feature                                    0x1A
#define HCI_ERR_SCO_Offset_Rejected                                           0x1B
#define HCI_ERR_SCO_Interval_Rejected                                         0x1C
#define HCI_ERR_SCO_Air_Mode_Rejected                                         0x1D
#define HCI_ERR_Invalid_LMP_Parameters                                        0x1E
#define HCI_ERR_Unspecified_Error                                             0x1F
#define HCI_ERR_Unsupported_LMP_Parameter                                     0x20
#define HCI_ERR_Role_Change_Not_Allowed                                       0x21
#define HCI_ERR_LMP_Response_Timeout                                          0x22
#define HCI_ERR_LMP_Error_Transaction_Collision                               0x23
#define HCI_ERR_LMP_PDU_Not_Allowed                                           0x24
#define HCI_ERR_Encryption_Mode_Not_Acceptable                                0x25
#define HCI_ERR_Unit_Key_Used                                                 0x26
#define HCI_ERR_QoS_Not_Supported                                             0x27
#define HCI_ERR_Instant_Passed                                                0x28
#define HCI_ERR_Pairing_With_Unit_Key_Not_Supported                           0x29



#endif


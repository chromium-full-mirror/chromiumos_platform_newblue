#ifndef _HCI_INT_H_
#define _HCI_INT_H_

#include "util.h"

#define HCI_LAP_Unlimited_Inquiry                                0x9E8B33
#define HCI_LAP_Limited_Inquiry                                  0x9E8B00

#define PAGE_STATE_INQUIRY                                       1
#define PAGE_STATE_PAGE                                          2




#define HCI_PKT_TYP_NO_2_DH1                                     0x0002 /* BT 2.1+ */
#define HCI_PKT_TYP_NO_3_DH1                                     0x0004 /* BT 2.1+ */
#define HCI_PKT_TYP_DM1                                          0x0008 /* BT 1.2+ */
#define HCI_PKT_TYP_DH1                                          0x0010 /* BT 1.2+ */
#define HCI_PKT_TYP_NO_2_DH3                                     0x0100 /* BT 2.1+ */
#define HCI_PKT_TYP_NO_3_DH3                                     0x0200 /* BT 2.1+ */
#define HCI_PKT_TYP_DM3                                          0x0400 /* BT 1.2+ */
#define HCI_PKT_TYP_DH3                                          0x0800 /* BT 1.2+ */
#define HCI_PKT_TYP_NO_2_DH5                                     0x1000 /* BT 2.1+ */
#define HCI_PKT_TYP_NO_3_DH5                                     0x1000 /* BT 2.1+ */
#define HCI_PKT_TYP_DM5                                          0x4000 /* BT 1.2+ */
#define HCI_PKT_TYP_DH5                                          0x8000 /* BT 1.2+ */

#define HCI_PKT_TYP_SCO_HV1                                      0x0001 /* BT 1.2+ */
#define HCI_PKT_TYP_SCO_HV2                                      0x0002 /* BT 1.2+ */
#define HCI_PKT_TYP_SCO_HV3                                      0x0004 /* BT 1.2+ */
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

#define HCI_EVENT_INQUIRY_COMPLETE                               0x0000000000000001ULL /* BT 1.2+ */
#define HCI_EVENT_INQUIRY_RESULT                                 0x0000000000000002ULL /* BT 1.2+ */
#define HCI_EVENT_CONN_COMPLETE                                  0x0000000000000004ULL /* BT 1.2+ */
#define HCI_EVENT_CONN_REQUEST                                   0x0000000000000008ULL /* BT 1.2+ */
#define HCI_EVENT_DISCONNECTION_COMPLETE                         0x0000000000000010ULL /* BT 1.2+ */
#define HCI_EVENT_AUTH_COMPLETE                                  0x0000000000000020ULL /* BT 1.2+ */
#define HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE                   0x0000000000000040ULL /* BT 1.2+ */
#define HCI_EVENT_ENCR_CHANGE                                    0x0000000000000080ULL /* BT 1.2+ */
#define HCI_EVENT_CHANGE_CONN_LINK_KEY_COMPLETE                  0x0000000000000100ULL /* BT 1.2+ */
#define HCI_EVENT_MASTER_LINK_KEY_COMPLETE                       0x0000000000000200ULL /* BT 1.2+ */
#define HCI_EVENT_READ_REMOTE_SUPPORTED_FEATURES_COMPLETE        0x0000000000000400ULL /* BT 1.2+ */
#define HCI_EVENT_READ_REMOTE_VERSON_INFO_COMPLETE               0x0000000000000800ULL /* BT 1.2+ */
#define HCI_EVENT_QOS_SETUP_COMPLETE                             0x0000000000001000ULL /* BT 1.2+ */
#define HCI_EVENT_HARDWARE_ERROR                                 0x0000000000008000ULL /* BT 1.2+ */
#define HCI_EVENT_FLUSH_OCCURED                                  0x0000000000010000ULL /* BT 1.2+ */
#define HCI_EVENT_ROLE_CHANGE                                    0x0000000000020000ULL /* BT 1.2+ */
#define HCI_EVENT_MODE_CHANGE                                    0x0000000000080000ULL /* BT 1.2+ */
#define HCI_EVENT_RETURN_LINK_KEYS                               0x0000000000100000ULL /* BT 1.2+ */
#define HCI_EVENT_PIN_CODE_REQUEST                               0x0000000000200000ULL /* BT 1.2+ */
#define HCI_EVENT_LINK_KEY_REQUEST                               0x0000000000400000ULL /* BT 1.2+ */
#define HCI_EVENT_LINK_KEY_NOTIFICATION                          0x0000000000800000ULL /* BT 1.2+ */
#define HCI_EVENT_LOOPBACK_COMMAND                               0x0000000001000000ULL /* BT 1.2+ */
#define HCI_EVENT_DATA_BUFFER_OVERFLOW                           0x0000000002000000ULL /* BT 1.2+ */
#define HCI_EVENT_MAX_SLOTS_CHANGE                               0x0000000004000000ULL /* BT 1.2+ */
#define HCI_EVENT_READ_CLOCK_OFFSET_COMPLETE                     0x0000000008000000ULL /* BT 1.2+ */
#define HCI_EVENT_CONN_PACKET_TYPE_CHANGED                       0x0000000010000000ULL /* BT 1.2+ */
#define HCI_EVENT_QOS_VIOLATION                                  0x0000000020000000ULL /* BT 1.2+ */
#define HCI_EVENT_PAGE_SCAN_MODE_CHANGE                          0x0000000040000000ULL /* BT 1.2+, obsolete @ BT1.2+ */
#define HCI_EVENT_PAGE_SCAN_REPETITION_MODE_CHANGE               0x0000000080000000ULL /* BT 1.2+ */
#define HCI_EVENT_FLOW_SPEC_COMPLETE                             0x0000000100000000ULL /* BT 1.2+ */
#define HCI_EVENT_INQUIRY_RESULT_WITH_RSSI                       0x0000000200000000ULL /* BT 1.2+ */
#define HCI_EVENT_READ_REMOTE_EXTENDED_FETURES_COMPLETE          0x0000000400000000ULL /* BT 1.2+ */
#define HCI_EVENT_SYNC_CONN_COMPLETE_EVENT                       0x0000080000000000ULL /* BT 1.2+ */
#define HCI_EVENT_SYNC_CONN_CHANGED_EVENT                        0x0000100000000000ULL /* BT 1.2+ */
#define HCI_EVENT_ALL_BT_1_2                                     0x00001FFFFFFFFFFFULL /* also the default */
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
#define HCI_EVENT_P2_DISCONNECTION_LOGICAL_LINK_COMPLETE         0x0000000000000040ULL /* BT 3.0+ */
#define HCI_EVENT_P2_FLOW_SPEC_MODIFY_COMPLETE                   0x0000000000000080ULL /* BT 3.0+ */
#define HCI_EVENT_P2_NUMBER_OF_COMPLETED_DATA_BLOCKS             0x0000000000000100ULL /* BT 3.0+ */
#define HCI_EVENT_P2_AMP_START_TEST                              0x0000000000000200ULL /* BT 3.0+ */
#define HCI_EVENT_P2_AMP_TEST_END                                0x0000000000000400ULL /* BT 3.0+ */
#define HCI_EVENT_P2_AMP_RECEIVER_REPORT                         0x0000000000000800ULL /* BT 3.0+ */
#define HCI_EVENT_P2_SHORT_RANGE_MODE_CHANGE_COMPLETE            0x0000000000001000ULL /* BT 3.0+ */
#define HCI_EVENT_P2_AMP_STATUS_CHANGE                           0x0000000000002000ULL /* BT 3.0+ */

#define HCI_LE_EVENT_CONN_COMPLETE                               0x0000000000000001ULL /* BT 4.0+ */
#define HCI_LE_EVENT_ADV_REPORT                                  0x0000000000000002ULL /* BT 4.0+ */
#define HCI_LE_EVENT_CONN_UPDATE_COMPLETE                        0x0000000000000004ULL /* BT 4.0+ */
#define HCI_LE_EVENT_READ_REMOTE_USED_FEATURES_CMPLETE           0x0000000000000008ULL /* BT 4.0+ */
#define HCI_LE_EVENT_LTK_REQUEST                                 0x0000000000000010ULL /* BT 4.0+ */




#define HCI_OGF_Link_Control                                     1


/* ==== BT 1.2 ==== */

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
    uint16_t clockOffset; /* >> 2 and possibly | HCI_CLOCK_OFST_VALID */
    uint8_t allowRoleSwitch;
} __packed;
#define HCI_CLOCK_OFST_VALID  0x8000

#define HCI_CMD_Disconnect                                       0x0006
struct hciDisconnect {
    uint16_t conn;
    uint8_t reason;
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
    uint8_t sbz;
    uint16_t clockOffset; /* >> 2 and possibly | HCI_CLOCK_OFST_VALID */
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
struct hciReadclockOffset {
    uint16_t conn;
} __packed;

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





#define HCI_OGF_Link_Policy                                      2


/* ==== BT 1.2 ==== */

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


/* ==== BT 1.2 ==== */

#define HCI_CMD_Set_Event_Mask                                   0x0001
struct hciCmdSetEventMask {
    uint64_t mask; /* bitmask of HCI_EVENT_* */
} __packed;

#define HCI_CMD_Reset                                            0x0003
#define HCI_CMD_Set_Event_Filter                                 0x0005
#define HCI_CMD_Flush                                            0x0008
#define HCI_CMD_Read_PIN_Type                                    0x0009
#define HCI_CMD_Write_PIN_Type                                   0x000A
struct hciWritePinType {
    uint8_t isFixed;
} __packed;

#define HCI_CMD_Create_New_Unit_Key                              0x000B
#define HCI_CMD_Read_Stored_Link_Key                             0x000D
#define HCI_CMD_Write_Stored_Link_Key                            0x0011
#define HCI_CMD_Delete_Stored_Link_Key                           0x0012
#define HCI_CMD_Write_Local_Name                                 0x0013
struct hciCmdWriteLocalName {
    char name[248];
} __packed;

#define HCI_CMD_Read_Local_Name                                  0x0014
#define HCI_CMD_Read_Connection_Accept_Timeout                   0x0015
#define HCI_CMD_Write_Connection_Accept_Timeout                  0x0016
#define HCI_CMD_Read_Page_Timeout                                0x0017
#define HCI_CMD_Write_Page_Timeout                               0x0018
struct hciWritePageTimeout {
    uint16_t timeout;
} __packed;

#define HCI_CMD_Read_Scan_Enable                                 0x0019
#define HCI_CMD_Write_Scan_Enable                                0x001A
struct hciWriteScanEnable {
    uint8_t state;
} __packed;
# define HCI_SCAN_ENABLE_INQUIRY 0x01 /* discoverable */
# define HCI_SCAN_ENABLE_PAGE    0x02 /* connectable */

#define HCI_CMD_Read_Page_Scan_Activity                          0x001B
#define HCI_CMD_Write_Page_Scan_Activity                         0x001C
#define HCI_CMD_Read_Inquiry_Scan_Activity                       0x001D
#define HCI_CMD_Write_Inquiry_Scan_Activity                      0x001E
#define HCI_CMD_Authentication_Enable                            0x001F
#define HCI_CMD_Write_Authentication_Enable                      0x0020
struct hciWriteAuthEnable {
    uint8_t authRequired;
} __packed;

#define HCI_CMD_Read_Encryption_Mode                             0x0021
#define HCI_CMD_Write_Encryption_Mode                            0x0022
#define HCI_CMD_Read_Class_Of_Device                             0x0023
#define HCI_CMD_Write_Class_Of_Device                            0x0024
struct hciCmdWriteClassOfDevice {
    uint8_t cls[3]; /* 24-bit value */
} __packed;

#define HCI_CMD_Read_Voice_Setting                               0x0025
#define HCI_CMD_Voice_Setting                                    0x0026
#define HCI_CMD_Read_Automatic_Flush_Timeout                     0x0027
#define HCI_CMD_Write_Automatic_Flush_Timeout                    0x0028
struct hciWriteAutoFlushTimeout {
    uint16_t conn;
    uint16_t timeout;
} __packed;

#define HCI_CMD_Read_Num_Broadcast_Retransmissions               0x0029
#define HCI_CMD_Write_Num_Broadcast_Retransmissions              0x002A
#define HCI_CMD_Read_Hold_Mode_Activity                          0x002B
#define HCI_CMD_Write_Hold_Mode_Activity                         0x002C
#define HCI_CMD_Transmit_Power_Level                             0x002D
#define HCI_CMD_Read_SCO_Flow_Control_Enable                     0x002E
#define HCI_CMD_SCO_Flow_Control_Enable                          0x002F
struct hciWriteSyncFlowCtrl {
    uint8_t syncFlowCtrlOn;
} __packed;

#define HCI_CMD_Set_Host_Controller_To_Host_Flow_Control         0x0031
#define HCI_CMD_Host_Buffer_Size                                 0x0033
#define HCI_CMD_Host_Number_Of_Completed_Packets                 0x0035
#define HCI_CMD_Read_Link_Supervision_Timeout                    0x0036
#define HCI_CMD_Write_Link_Supervision_Timeout                   0x0037
#define HCI_CMD_Read_Number_Of_Supported_IAC                     0x0038
#define HCI_CMD_Read_Current_IAC_LAP                             0x0039
#define HCI_CMD_Write_Current_IAC_LAP                            0x003A
#define HCI_CMD_Read_Page_Scan_Period_Mode                       0x003B
#define HCI_CMD_Write_Page_Scan_Period_Mode                      0x003C
#define HCI_CMD_Read_Page_Scan_Mode                              0x003D
#define HCI_CMD_Write_Page_Scan_Mode                             0x003E

#define HCI_CMD_Write_InquiryMode                                0x0045
struct hciWriteInquryMode {
    uint8_t inqMode;
} __packed;
#define INQ_MODE_STD    0 /* normal mode @ BT 1.2+  */
#define INQ_MODE_RSSI   1 /* with RSSI   @ BT 1.2+ */
#define INQ_MODE_EIR    2 /* with EIR    @ BT 2.1+ */


/* ==== BT 2.1 ==== */

#define HCI_CMD_Write_Extended_Inquiry_Response                  0x0057
struct hciWriteEIR {
    uint8_t useFec;
    uint8_t data[240];
} __packed;

#define HCI_CMD_Write_Simple_Pairing_Mode                        0x0056
struct hciWriteSimplePairingMore {
    uint8_t useSsp;
} __packed;


/* ==== BT 3.0 ==== */

#define HCI_CMD_Set_Event_Mask_Page_2                            0x0063
struct hciSetEventMaskPage2 {
    uint64_t evens; /* bitmask of HCI_EVENT_P2_* */
} __packed;

#define HCI_CMD_Read_Location_Data                               0x0064





#define HCI_OGF_Informational                                    4


/* ==== BT 1.2 ==== */

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





#define HCI_OGF_Status                                           5


/* == BT 1.2 == */

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



#define HCI_EVT_Inquiry_Complete_Event                           0x01
#define HCI_EVT_Inquiry_Result_Event                             0x02
#define HCI_EVT_Connection_Complete_Event                        0x03
#define HCI_EVT_Connection_Request_Event                         0x04
#define HCI_EVT_Disconnection_Complete_Event                     0x05
#define HCI_EVT_Authentication_Complete_Event                    0x06
#define HCI_EVT_Remote_Name_Request_Complete_Event               0x07
#define HCI_EVT_Encryption_Change_Event                          0x08
#define HCI_EVT_Change_Connection_Link_Key_Complete_Event        0x09
#define HCI_EVT_Master_Link_Key_Complete_Event                   0x0A
#define HCI_EVT_Read_Remote_Supported_Features_Complete_Event    0x0B
#define HCI_EVT_Read_Remote_Version_Complete_Event               0x0C
#define HCI_EVT_QOS_Setup_Complete_Event                         0x0D
#define HCI_EVT_Command_Complete_Event                           0x0E
struct hciEvtCmdComplete {
    uint8_t numCmdCredits;
    uint16_t opcode;
} __packed;

#define HCI_EVT_Command_Status_Event                             0x0F
struct hciEvtCmdStatus {
    uint8_t status;
    uint8_t numCmdCredits;
    uint16_t opcode;
} __packed;

#define HCI_EVT_Hardware_Error_Event                             0x10
#define HCI_EVT_Flush_Occured_event                              0x11
#define HCI_EVT_Role_Change_Event                                0x12
#define HCI_EVT_Number_Of_Completed_Packets_Event                0x13
#define HCI_EVT_Mode_Change_Event                                0x14
#define HCI_EVT_Return_Link_Keys_Event                           0x15
#define HCI_EVT_PIN_Code_Request_Event                           0x16
#define HCI_EVT_Link_Key_Request_Event                           0x17
#define HCI_EVT_Link_Key_Notification_Event                      0x18
#define HCI_EVT_Loopback_Command_Event                           0x19
#define HCI_EVT_Data_Buffer_Overflow_Event                       0x1A
#define HCI_EVT_Max_Slots_Change_Event                           0x1B
#define HCI_EVT_Read_Clock_Offset_Complete_Event                 0x1C
#define HCI_EVT_Connection_Packet_Type_Changed_Event             0x1D
#define HCI_EVT_QoS_Violation_Event                              0x1E
#define HCI_EVT_Page_Scan_Mode_Change_Event                      0x1F
#define HCI_EVT_Page_Scan_Repetition_Mode_Change_Event           0x20

#define HCI_EVT_IO_Capability_Request_Event                      0x31
#define HCI_EVT_IO_Capability_Response_Event                     0x32
#define HCI_EVT_User_Confirmation_Request_Event                  0x33
#define HCI_EVT_User_Passkey_Request_Event                       0x34
#define HCI_EVT_Remote_OOB Data_Request_Event                    0x35
#define HCI_EVT_Simple_Pairing_Complete_Event                    0x36
#define HCI_EVT_User_Passkey_Notification_Event                  0x3B

#define HCI_EVT_LE_Meta_Event                                    0x3E
struct hciEvtLeMeta {
    uint8_t subevent;
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


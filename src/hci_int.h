#ifndef _HCI_INT_H_
#define _HCI_INT_H_

#include "util.h"

#define HCI_LAP_Unlimited_Inquiry                                0x9E8B33
#define HCI_LAP_Limited_Inquiry                                  0x9E8B00

#define PAGE_STATE_INQUIRY                                       1
#define PAGE_STATE_PAGE                                          2


#define HCI_OGF_Link_Control                                     1

#define HCI_CMD_Inquiry                                          0x0001
#define HCI_CMD_Inquiry_Cancel                                   0x0002
#define HCI_CMD_Periodic_Inquiry_Mode                            0x0003
#define HCI_CMD_Exit_Periodic_Inquiry_Mode                       0x0004
#define HCI_CMD_Create_Connection                                0x0005
#define HCI_CMD_Disconnect                                       0x0006
#define HCI_CMD_Add_SCO_Connection                               0x0007
#define HCI_CMD_Accept_Connection_Request                        0x0009
#define HCI_CMD_Reject_Connection_Request                        0x000A
#define HCI_CMD_Link_Key_Request_Reply                           0x000B
#define HCI_CMD_Link_Key_Request_Negative_Reply                  0x000C
#define HCI_CMD_PIN_Code_Request_Reply                           0x000D
#define HCI_CMD_PIN_Code_Request_Negative_Reply                  0x000E
#define HCI_CMD_Change_Connection_Packet_Type                    0x000F
#define HCI_CMD_Authentication_Requested                         0x0011
#define HCI_CMD_Set_Connection_Encryption                        0x0013
#define HCI_CMD_Change_Connection_Link_Key                       0x0015
#define HCI_CMD_Master_Link_Key                                  0x0017
#define HCI_CMD_Remote_Name_Request                              0x0019
#define HCI_CMD_Read_Remote_Supported_Features                   0x001B
#define HCI_CMD_Read_Remote_Version_Information                  0x001D
#define HCI_CMD_Read_Clock_Offset                                0x001F

#define HCI_CMD_IO_Capability_Request_Reply                      0x002B
#define HCI_CMD_User_Confirmation_Request_Reply                  0x002C
#define HCI_CMD_User_Confirmation_Request_Negative_Reply         0x002D
#define HCI_CMD_User_Passkey_Request_Reply                       0x002E
#define HCI_CMD_User_Passkey_Request_Negative_Reply              0x002F
#define HCI_CMD_Remote_OOB_Data_Request_Reply                    0x0030
#define HCI_CMD_Remote_OOB_Data_Request_Negative_Reply           0x0033
#define HCI_CMD_IO_Capability_Request_Negative_Reply             0x0034

#define HCI_OGF_Policy                                           2

#define HCI_CMD_Hold_Mode                                        0x0001
#define HCI_CMD_Sniff_Mode                                       0x0003
#define HCI_CMD_Exit_Sniff_Mode                                  0x0004
#define HCI_CMD_Park_Mode                                        0x0005
#define HCI_CMD_Exit_Park_Mode                                   0x0006
#define HCI_CMD_QoS_Setup                                        0x0007
#define HCI_CMD_Role_Discovery                                   0x0009
#define HCI_CMD_Switch_Role                                      0x000B
#define HCI_CMD_Read_Link_Policy_Settings                        0x000C
#define HCI_CMD_Write_Link_Policy_Settings                       0x000D


#define HCI_OGF_Controller_and_Baseband                          3

#define HCI_CMD_Set_Event_Mask                                   0x0001
#define HCI_CMD_Reset                                            0x0003
#define HCI_CMD_Set_Event_Filter                                 0x0005
#define HCI_CMD_Flush                                            0x0008
#define HCI_CMD_Read_PIN_Type                                    0x0009
#define HCI_CMD_Write_PIN_Type                                   0x000A
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
#define HCI_CMD_Read_Scan_Enable                                 0x0019
#define HCI_CMD_Write_Scan_Enable                                0x001A
#define HCI_CMD_Read_Page_Scan_Activity                          0x001B
#define HCI_CMD_Write_Page_Scan_Activity                         0x001C
#define HCI_CMD_Read_Inquiry_Scan_Activity                       0x001D
#define HCI_CMD_Write_Inquiry_Scan_Activity                      0x001E
#define HCI_CMD_Authentication_Enable                            0x001F
#define HCI_CMD_Write_Authentication_Enable                      0x0020
#define HCI_CMD_Read_Encryption_Mode                             0x0021
#define HCI_CMD_Write_Encryption_Mode                            0x0022
#define HCI_CMD_Read_Class_Of_Device                             0x0023
#define HCI_CMD_Write_Class_Of_Device                            0x0024
#define HCI_CMD_Read_Voice_Setting                               0x0025
#define HCI_CMD_Voice_Setting                                    0x0026
#define HCI_CMD_Read_Automatic_Flush_Timeout                     0x0027
#define HCI_CMD_Write_Automatic_Flush_Timeout                    0x0028
#define HCI_CMD_Read_Num_Broadcast_Retransmissions               0x0029
#define HCI_CMD_Write_Num_Broadcast_Retransmissions              0x002A
#define HCI_CMD_Read_Hold_Mode_Activity                          0x002B
#define HCI_CMD_Write_Hold_Mode_Activity                         0x002C
#define HCI_CMD_Transmit_Power_Level                             0x002D
#define HCI_CMD_Read_SCO_Flow_Control_Enable                     0x002E
#define HCI_CMD_SCO_Flow_Control_Enable                          0x002F
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

#define HCI_CMD_Write_Simple_Pairing_Mode                        0x0056


#define HCI_OGF_Informational                                    4

#define HCI_CMD_Read_Local_Version_Information                   0x0001
#define HCI_CMD_Read_Local_Supported_Features                    0x0003
#define HCI_CMD_Read_Buffer_Size                                 0x0005
#define HCI_CMD_Read_Country_Code                                0x0007
#define HCI_CMD_Read_BD_ADDR                                     0x0009




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


#ifndef _INCLUDED_FROM_HCI_H_
#error "do not ever include this file anywhere"
#endif


/*
 * This generates simple SYNC functions for BT commands.
 * We need a lot of them, so we play with macros here.
 * It's either this, magic number mess, or hundreds of
 * functions. Deal with it.
 *
 *
 *
 * for all generated funcs this applies:
 *
 * RETURN:   status byte or negatives on error
 * NOTES:    sync
 *
 * WRITE funcs just return status value, the reset of the event is discarded. expects cmd complete event
 * READ funcs take "evt" param to save event to. expects cmd complete event
 * NOCMDPARAMS funcs do not have params. expects cmd complete event
 * W_STATUS - expects cmd status event
 */

#define HCI_SIMPLY_SYNC_CMD_W_STATUS(name, params, ogf, ocf)                                                         \
                                           static int hci##name##Sync params                                         \
                                           {                                                                         \
                                               struct hci##name cmdD;                                                \
                                               struct hci##name *cmd = &cmdD;                                        \
                                               const uint32_t cmdSz = sizeof(*cmd);                                  \
                                               struct hciEvtCmdStatus evtD;                                          \
                                               struct hciEvtCmdStatus *evt = &evtD;                                  \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = true;                                           \
                                               uint32_t _len = 0;

#define HCI_SIMPLY_SYNC_WRITE(name, params, ogf, ocf)                                                                \
                                           static int hci##name##Sync params                                         \
                                           {                                                                         \
                                               struct hci##name cmdD;                                                \
                                               struct hci##name *cmd = &cmdD;                                        \
                                               const uint32_t cmdSz = sizeof(*cmd);                                  \
                                               struct hciCmpl##name evtD;                                            \
                                               struct hciCmpl##name *evt = &evtD;                                    \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = false;                                          \
                                               uint32_t _len = 0;

#define HCI_SIMPLY_SYNC_WRITE_NOCMDPARAMS(name, ogf, ocf)                                                            \
                                           static int hci##name##Sync (void)                                         \
                                           {                                                                         \
                                               struct hci##name *cmd = NULL;                                         \
                                               const uint32_t cmdSz = 0;                                             \
                                               struct hciCmpl##name evtD;                                            \
                                               struct hciCmpl##name *evt = &evtD;                                    \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = false;                                          \
                                               uint32_t _len = 0;

#define HCI_SIMPLY_SYNC_READ(name, params, ogf, ocf)                                                                 \
                                           static int hci##name##Sync params                                         \
                                           {                                                                         \
                                               struct hci##name cmdD;                                                \
                                               struct hci##name *cmd = &cmdD;                                        \
                                               const uint32_t cmdSz = sizeof(*cmd);                                  \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = false;                                          \
                                               uint32_t _len = 0;

#define HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(name, params, ogf, ocf)                                                     \
                                           static int hci##name##Sync params                                         \
                                           {                                                                         \
                                               struct hci##name *cmd = NULL;                                         \
                                               const uint32_t cmdSz = 0;                                             \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = false;                                          \
                                               uint32_t _len = 0;

#define REQUIRE_VERSION(_v)                    if (mBtVer < _v) {                                                    \
                                                   loge("attempt to call 0x%02X.%04X on v%u\n", _ogf, _ocf, mBtVer); \
                                                   return -2;                                                        \
                                               }

#define REQUIRE_FEATURE(_page, _v)             if (!(mLocalFtrs[_page] & _v)) {                                       \
                                                   loge("attempt to call 0x%02X.%04X w/o feature\n", _ogf, _ocf);    \
                                                   return -2;                                                        \
                                               }

#define REQUIRE_LE_FEATURE(_v)                 if (!(mLocalLeFtrs & _v)) {                                           \
                                                   loge("attempt to call 0x%02X.%04X w/o feature\n", _ogf, _ocf);    \
                                                   return -2;                                                        \
                                               }


#define HCI_COPY_IN_U64(name)                  utilSetLE64(&cmd->name, name);
#define HCI_COPY_IN_U32(name)                  utilSetLE32(&cmd->name, name);
#define HCI_COPY_IN_U24(name)                  utilSetLE24(&cmd->name, name);
#define HCI_COPY_IN_U16(name)                  utilSetLE16(&cmd->name, name);
#define HCI_COPY_IN_U8(name)                   utilSetLE8(&cmd->name, name);
#define HCI_COPY_IN_BOOL(name)                 utilSetLE8(&cmd->name, name ? 1 : 0);
#define HCI_COPY_IN_MAC(name)                  memcpy(cmd->name, name, sizeof(cmd->name));
#define HCI_COPY_IN_DATA(name,len)             if (len > sizeof(cmd->name)) {                                        \
                                                   logw("Truncating "#name" to %ub\n",(int)sizeof(cmd->name));       \
                                                   len = sizeof(cmd->name);                                          \
                                               }                                                                     \
                                               memcpy(cmd->name, name, len);

#define HCI_SIMPLY_SYNC_END                    if (!want_sta && !hciCmdSubmitSimpleWithCompleteSync(_ogf, _ocf, cmd, \
                                                      cmdSz, evt, sizeof(*evt)))                                     \
                                                   return -1;                                                        \
                                               if (want_sta && !hciCmdSubmitSimpleWithStatusSync(_ogf, _ocf, cmd,    \
                                                      cmdSz, (struct hciEvtCmdStatus*)evt))                          \
                                                   return -1;                                                        \
                                               return (unsigned)utilGetLE8(&evt->status);                            \
                                           }
/*
 * FUNCTION: hciLeSetAdvParamsSync
 * USE:      set adv params
 * PARAMS:   advIntervalMin - minimum adv interval (in units of 0.625ms)
 *           advIntervalMax - maximum adv interval (in units of 0.625ms)
 *           advType - adv type: HCI_ADV_TYPE_ADV_*
 *           ownAddressType - own addr type: HCI_ADV_OWN_ADDR_TYPE_*
 *           directRandomAddress - set to 1 if direct adv to random addr, else ignored
 *           directAddr - if direct adv, the mac address of target, else ignored
 *           advChannelMap - channels to use: bitmask of HCI_ADV_CHAN_MAP_USE_*
 *           advFilterPolicy - filter policy: HCI_ADV_FILTER_POL_*
 */
HCI_SIMPLY_SYNC_WRITE(LeSetAdvParams, (uint16_t advIntervalMin, uint16_t advIntervalMax, uint8_t advType, uint8_t ownAddressType, uint8_t directRandomAddress, const uint8_t *directAddr, uint8_t advChannelMap, uint8_t advFilterPolicy), HCI_OGF_LE, HCI_CMD_LE_Set_Adv_Params)
    REQUIRE_VERSION(HCI_VERSION_4_0);
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
    HCI_COPY_IN_U16(advIntervalMin);
    HCI_COPY_IN_U16(advIntervalMax);
    HCI_COPY_IN_U8(advType);
    HCI_COPY_IN_U8(ownAddressType);
    HCI_COPY_IN_U8(directRandomAddress);
    HCI_COPY_IN_MAC(directAddr);
    HCI_COPY_IN_U8(advChannelMap);
    HCI_COPY_IN_U8(advFilterPolicy);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciSetEventMaskSync
 * USE:      Set event mask
 * PARAMS:   mask - the wanted mask
 */
HCI_SIMPLY_SYNC_WRITE(SetEventMask, (uint64_t mask), HCI_OGF_Controller_and_Baseband, HCI_CMD_Set_Event_Mask)
    HCI_COPY_IN_U64(mask)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciSetEventMaskPage2Sync
 * USE:      Set event mask page 2
 * PARAMS:   mask - the wanted mask
 */
HCI_SIMPLY_SYNC_WRITE(SetEventMaskPage2, (uint64_t mask), HCI_OGF_Controller_and_Baseband, HCI_CMD_Set_Event_Mask_Page_2)
    REQUIRE_VERSION(HCI_VERSION_3_0)
    HCI_COPY_IN_U64(mask)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWritePinTypeSync
 * USE:      Set whether we have a fixed or variable PIN
 * PARAMS:   isFixed - is the pin fixed?
 */
HCI_SIMPLY_SYNC_WRITE(WritePinType, (bool isFixed), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_PIN_Type)
    HCI_COPY_IN_BOOL(isFixed)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWritePageTimeoutSync
 * USE:      Set how long we try to connect at link layer
 * PARAMS:   timeout - the timeout in unit of 0.625ms
 */
HCI_SIMPLY_SYNC_WRITE(WritePageTimeout, (uint16_t timeout), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Page_Timeout)
    HCI_COPY_IN_U16(timeout)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteSyncFlowCtrlEnableSync
 * USE:      Set whether we get command complete packets for SCO data
 * PARAMS:   syncFlowCtrlOn - set if we will get those packets, false else
 */
HCI_SIMPLY_SYNC_WRITE(WriteSyncFlowCtrlEnable, (bool syncFlowCtrlOn), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_SCO_Flow_Control_Enable)
    HCI_COPY_IN_BOOL(syncFlowCtrlOn)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteConnAcceptTimeoutSync
 * USE:      Set how long after a connection request comes in taht we auto-reject it
 * PARAMS:   timeout - timeout in units of 0.625ms
 */
HCI_SIMPLY_SYNC_WRITE(WriteConnAcceptTimeout, (uint16_t timeout), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Connection_Accept_Timeout)
    HCI_COPY_IN_U16(timeout)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteInquiryModeSync
 * USE:      Set what kind of inquiry result we want
 * PARAMS:   inqMode - the result format (INQ_MODE_*)
 */
HCI_SIMPLY_SYNC_WRITE(WriteInquiryMode, (uint8_t inqMode), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Inquiry_Mode)
    REQUIRE_VERSION(HCI_VERSION_1_2)
    HCI_COPY_IN_U8(inqMode)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteSimplePairingModeSync
 * USE:      Enable/disable SSP
 * PARAMS:   sspOn - use ssp?
 */
HCI_SIMPLY_SYNC_WRITE(WriteSimplePairingMode, (bool useSsp), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Simple_Pairing_Mode)
    REQUIRE_VERSION(HCI_VERSION_2_1)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_SSP)
    HCI_COPY_IN_BOOL(useSsp)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteLeHostSupportedSync
 * USE:      Enable/disable LE
 * PARAMS:   leSupportedHost - does host support LE?
 *           simultaneousLeHost - does host support simultaneous LE/EDR to same device
 */
HCI_SIMPLY_SYNC_WRITE(WriteLeHostSupported, (uint8_t leSupportedHost, uint8_t simultaneousLeHost), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_LE_Host_Supported)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER)

    if (simultaneousLeHost) {
        REQUIRE_FEATURE(0, HCI_LMP_FTR_SIMUL_LE_EDR_CAPABLE_CONTROLLER)
    }

    HCI_COPY_IN_BOOL(leSupportedHost)
    HCI_COPY_IN_BOOL(simultaneousLeHost)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWritePageScanActivitySync
 * USE:      Set connectivity settings (page scan)
 * PARAMS:   scanInterval - the scan interval
 *           scanWindow - the scan window
 */
HCI_SIMPLY_SYNC_WRITE(WritePageScanActivity, (uint16_t scanInterval, uint16_t scanWindow), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Page_Scan_Activity)
    HCI_COPY_IN_U16(scanInterval)
    HCI_COPY_IN_U16(scanWindow)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteInquiryScanActivitySync
 * USE:      Set discoverability settings (inquiry scan)
 * PARAMS:   scanInterval - the scan interval
 *           scanWindow - the scan window
 */
HCI_SIMPLY_SYNC_WRITE(WriteInquiryScanActivity, (uint16_t scanInterval, uint16_t scanWindow), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Inquiry_Scan_Activity)
    HCI_COPY_IN_U16(scanInterval)
    HCI_COPY_IN_U16(scanWindow)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciReadLocalVersionSync
 * USE:      read local version
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(ReadLocalVersion, (struct hciCmplReadLocalVersion *evt), HCI_OGF_Informational, HCI_CMD_Read_Local_Version_Information)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciReadLocalSupportedFeaturesSync
 * USE:      read local supported features
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(ReadLocalSupportedFeatures, (struct hciCmplReadLocalSupportedFeatures *evt), HCI_OGF_Informational, HCI_CMD_Read_Local_Supported_Features)
HCI_SIMPLY_SYNC_END


/*
 * FUNCTION: hciReadLocalExtendedFeaturesSync
 * USE:      read local extended features
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ(ReadLocalExtendedFeatures, (uint8_t page, struct hciCmplReadLocalExtendedFeatures *evt), HCI_OGF_Informational, HCI_CMD_Read_Local_Extended_Features)
    REQUIRE_VERSION(HCI_VERSION_2_1)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_EXTENDED_FEATURES)
    HCI_COPY_IN_U8(page)
HCI_SIMPLY_SYNC_END


/*
 * FUNCTION: hciReadBufferSizeSync
 * USE:      read local EDR buffer sizes
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(ReadBufferSize, (struct hciCmplReadBufferSize *evt), HCI_OGF_Informational, HCI_CMD_Read_Buffer_Size)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciLeReadBufferSizeSync
 * USE:      read local LE buffer sizes
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(LeReadBufferSize, (struct hciCmplLeReadBufferSize *evt), HCI_OGF_LE, HCI_CMD_LE_Read_Buffer_Size)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciReadBdAddrSync
 * USE:      read local MAC Address
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(ReadBdAddr, (struct hciCmplReadBdAddr *evt), HCI_OGF_Informational, HCI_CMD_Read_BD_ADDR)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciInquiryCancelSync
 * USE:      Cancel inquiry
 * PARAMS:   NONE
 */
HCI_SIMPLY_SYNC_WRITE_NOCMDPARAMS(InquiryCancel, HCI_OGF_Link_Control, HCI_CMD_Inquiry_Cancel)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciLeSetEventMaskSync
 * USE:      Set LE event mask
 * PARAMS:   events - the wanted event mask
 */
HCI_SIMPLY_SYNC_WRITE(LeSetEventMask, (uint64_t events), HCI_OGF_LE, HCI_CMD_LE_Set_Event_Mask)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
    HCI_COPY_IN_U64(events)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciLeSetScanEnableSync
 * USE:      Enable/disable LE scan
 * PARAMS:   scanOn - scan on/off
 *           filterDuplicates - this rarely works, do not use
 */
HCI_SIMPLY_SYNC_WRITE(LeSetScanEnable, (bool scanOn, bool filterDuplicates), HCI_OGF_LE, HCI_CMD_LE_Set_Scan_Enable)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
    HCI_COPY_IN_BOOL(scanOn)
    HCI_COPY_IN_BOOL(filterDuplicates)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciLeSetScanParamsSync
 * USE:      Set LE scan parameters
 * PARAMS:   activeScan - as per spec
 *           scanInterval - as per spec
 *           scanWindow - as per spec
 *           useOwnRandomAddr - as per spec
 *           onlyWhitelist - as per spec
 */
HCI_SIMPLY_SYNC_WRITE(LeSetScanParams, (uint8_t activeScan, uint16_t scanInterval, uint16_t scanWindow, bool useOwnRandomAddr, bool onlyWhitelist), HCI_OGF_LE, HCI_CMD_LE_Set_Scan_Parameters)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
    HCI_COPY_IN_U16(activeScan);
    HCI_COPY_IN_U16(scanInterval);
    HCI_COPY_IN_U16(scanWindow);
    HCI_COPY_IN_BOOL(useOwnRandomAddr);
    HCI_COPY_IN_BOOL(onlyWhitelist);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciLeReadNumberOfSupportedAdvertisingSetsSync
 * USE:      Read num adv sets we support
 * PARAMS:   None
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(LeReadNumberOfSupportedAdvertisingSets, (struct hciCmplLeReadNumSupportedAdvSets* evt), HCI_OGF_LE, HCI_CMD_LE_Read_Num_Supported_Adv_Sets)
    REQUIRE_VERSION(HCI_VERSION_5_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciVendorLeGetVendorCapabilitiesSync
 * USE:      Read android-vendor supported chip info
 * PARAMS:   None
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(VendorLeGetVendorCapabilities, (struct hciCmplVendorLeGetVendorCapabilities* evt), HCI_OGF_Vendor, HCI_CMD_Vendor_LE_Get_Vendor_Capabilities)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciLeReadLocalSupportedFeaturesSync
 * USE:      Read suported LE features locally
 * PARAMS:   *evt = the return event
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(LeReadLocalSupportedFeatures, (struct hciCmplLeReadLocalSupportedFeatures* evt), HCI_OGF_LE, HCI_CMD_LE_Read_Local_Supported_Features)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciRemoteNameRequestCancelSync
 * USE:      Cancel a remote name request
 * PARAMS:   mac - the mac
 */
HCI_SIMPLY_SYNC_WRITE(RemoteNameRequestCancel, (const uint8_t *mac), HCI_OGF_Link_Control, HCI_CMD_Remote_Name_Request_Cancel)
    HCI_COPY_IN_MAC(mac);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciInquirySync
 * USE:      Enable EDR scan
 * PARAMS:   lap - the lap to use
 *           inqLen - how long for (max is 0x30 -> 61.44 sec)
 *           numResp - max responses (0 = unlimited)
 */
HCI_SIMPLY_SYNC_CMD_W_STATUS(Inquiry, (uint32_t lap, uint8_t inqLen, uint8_t numResp), HCI_OGF_Link_Control, HCI_CMD_Inquiry)
    HCI_COPY_IN_U24(lap)
    HCI_COPY_IN_U8(inqLen)
    HCI_COPY_IN_U8(numResp)
HCI_SIMPLY_SYNC_END


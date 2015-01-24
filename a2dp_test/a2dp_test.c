#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "vendorLib.h"
#include "persist.h"
#include "timer.h"
#include "l2cap.h"
#include "a2dp.h"
#include "aapi.h"
#include "util.h"
#include "uuid.h"
#include "hci.h"
#include "log.h"
#include "sdp.h"
#include "bt.h"
#include "mt.h"

#include "audio.h"

#include <cutils/misc.h>

struct audio_context* audioContext = NULL;


static uint8_t gStatus = 0;
static sem_t* gSem;
static int gAlsaCard = 0;
static int gAlsaDevice = 0;
static int gAlsaPeriodSize = 1024;
static int gAlsaPeriodCount = 4;
static int gAlsaRate = 0;   // rate to use for ALSA, if we need to resample


static void opDoneCbk(void *cbkData, uint8_t status)
{
    sem_t *sem = (sem_t*)cbkData;

    gStatus = status;
    logi("status=%d\n", status);
    sem_post(sem);
}

static void setDiscoverableConnectable(bool discoverable, bool connectable) {
    logi("setting inquiry state discoverable: %d connectable: %d\n", discoverable, connectable);
    if (!hciSetDiscoverableConnectable(&discoverable, &connectable, opDoneCbk, gSem))
        loge("Failed to try to set discoverable\n");
    r_sem_wait(gSem);
    if(gStatus)
        loge("Failed to set discoverable/connectable. Sta = %d\n", gStatus);
}

static void a2dpStateCallback(void *userData, uint8_t state) {
    switch (state) {
    case A2DP_STATE_NONE:
        fprintf(stderr, "A2DP_STATE_NONE\n");
        break;
    case A2DP_STATE_OPEN:
        fprintf(stderr, "A2DP_STATE_OPEN\n");
        setDiscoverableConnectable(false, false);
        break;
    case A2DP_STATE_START:
        fprintf(stderr, "A2DP_STATE_START\n");
        break;
    case A2DP_STATE_STOP:
        fprintf(stderr, "A2DP_STATE_STOP\n");
        break;
    case A2DP_STATE_CLOSED:
        fprintf(stderr, "A2DP_STATE_CLOSED\n");
        setDiscoverableConnectable(true, true);
        break;
    }
}

static void a2dpParamsCallback(void *userData, unsigned int rate, uint8_t channelMode) {
    if (audioContext) {
        audio_close(audioContext);
        audioContext = NULL;
    }
    if (!audioContext) {
        unsigned int channels = 2;
        unsigned int bits = 16;
        unsigned int period_size = 1024;
        unsigned int period_count = 4;
        bool resample = false;

        audioContext = audio_open(gAlsaCard, gAlsaDevice, 2, rate, (gAlsaRate ? gAlsaRate : rate),
                16, gAlsaPeriodSize, gAlsaPeriodCount);
        if (!audioContext) {
            fprintf(stderr, "audio_open failed\n");
            abort();
        }
    }
}

static void a2dpDataCallback(void *userData, const uint16_t* samples, uint32_t sampleCount) {
    audio_play(audioContext, samples, sampleCount);
}

#define DEVICE_CLASS_SERVICE_RENDERING		0x040000	//PRINTER, SPEAKER
#define DEVICE_CLASS_SERVICE_AUDIO		0x200000	//SPEAKER, MIC, HEADSET
#define DEVICE_CLASS_SERVICE_INFORMATION	0x800000	//WEB-server, WAP-server

#define DEVICE_CLASS_MAJOR_SHIFT		8

#define DEVICE_CLASS_MAJOR_AV			4

#define DEVICE_CLASS_MINOR_AV_SHIFT		2
#define DEVICE_CLASS_MINOR_AV_PORTBL_AUDIO	7

static const struct a2dpSinkDescriptor a2dpService = {
    .stateCb = a2dpStateCallback,
    .paramsCb = a2dpParamsCallback,
    .dataCb = a2dpDataCallback,
};

static char* getNexusQMacAddress() {
    char cmdline[1024];
    char* addr_str;

    // Support for reading MAC address from kernel command line
    // This only works on Nexus Q
    FILE* file = fopen("/proc/cmdline", "r");
    if (!file) {
        fprintf(stderr, "could not open /proc/cmdline\n");
        return NULL;
    }
    fread(cmdline, 1, sizeof(cmdline), file);
    fclose(file);

    addr_str = strstr(cmdline, "board_steelhead_bluetooth.btaddr=");
    if (addr_str) {
        addr_str += strlen("board_steelhead_bluetooth.btaddr=");
        return strdup(addr_str);
    } else {
        return NULL;
    }
}

int main(int argc, char** argv)
{
    static uint8_t mac[6];
    int temp[6];
    const bool vTrue = true;
    const bool vFalse = false;
    unsigned i;
    int svc;
    sem_t sem;
    uniq_t h;
    char* addr_str = NULL;
    char* device_name = argv[0];

    // parse command line arguments
    argv++;
    while (*argv) {
        if (strcmp(*argv, "-c") == 0) {
            argv++;
            if (*argv)
                gAlsaCard = atoi(*argv);
        } else if (strcmp(*argv, "-d") == 0) {
            argv++;
            if (*argv)
                gAlsaDevice = atoi(*argv);
        } else if (strcmp(*argv, "-r") == 0) {
            argv++;
            if (*argv)
                gAlsaRate = atoi(*argv);
        } else if (strcmp(*argv, "-ps") == 0) {
            argv++;
            if (*argv)
                gAlsaPeriodSize = atoi(*argv);
        } else if (strcmp(*argv, "-pc") == 0) {
            argv++;
            if (*argv)
                gAlsaPeriodCount = atoi(*argv);
        } else if (strcmp(*argv, "-n") == 0) {
            argv++;
            if (*argv)
                device_name = *argv;
        } else if (strcmp(*argv, "-a") == 0) {
            argv++;
            if (*argv)
                addr_str = *argv;
        }
        if (*argv)
            argv++;
    }

    if (sem_init(&sem, 0, 0)) {
        loge("sem init failed\n");
        return -1;
    }
    gSem = &sem;

#ifdef TEST_DEBUG
    vendorLogEnable(true);
#endif

    if (!addr_str) {
        addr_str = getNexusQMacAddress();
    }
    if (!addr_str) {
        addr_str = "11:22:33:44:55:66";
    }
    if (sscanf(addr_str, "%x:%x:%x:%x:%x:%x",
        &temp[0], &temp[1], &temp[2], &temp[3], &temp[4], &temp[5]) == 6) {
        for (i = 0; i < 6; i++) {
            mac[i] = temp[i];
        }
    } else {
        fprintf(stderr, "could not parse MAC addresss %s\n", addr_str);
        abort();
    }

    persistLoad();
    timersInit();
    if (!hciUp(mac, HCI_DISP_CAP_NONE)) {
        loge("HCI up fail\n");
        return -1;
    }

    logi("setting name\n");
    if (!hciSetLocalName(device_name, opDoneCbk, &sem))
        loge("Failed to try to set name\n");
    r_sem_wait(&sem);
    if(gStatus)
        loge("Failed to set name. Sta = %d\n", gStatus);


    logi("setting class\n");
    if (!hciSetDeviceClass(DEVICE_CLASS_SERVICE_AUDIO | DEVICE_CLASS_SERVICE_RENDERING |
		DEVICE_CLASS_SERVICE_INFORMATION | (DEVICE_CLASS_MAJOR_AV << DEVICE_CLASS_MAJOR_SHIFT) |
		(DEVICE_CLASS_MINOR_AV_PORTBL_AUDIO << DEVICE_CLASS_MINOR_AV_SHIFT), opDoneCbk, &sem))
        loge("Failed to try to set device class\n");
    r_sem_wait(&sem);
    if(gStatus)
        loge("Failed to set device class. Sta = %d\n", gStatus);


    setDiscoverableConnectable(true, true);

    l2cInit();
    sdpInit();
    a2dpSinkInit(&a2dpService);

    while (1) sleep(1000);

    a2dpSinkDeinit();
    sdpDeinit();
    l2cDeinit();
    hciDown();
    timersDeinit();
    persistStore();

    logi("done\n");

    return 0;
}

void aapiAdapterStateChanged(bool on)
{
    logd("aapiAdapterStateChanged(%d)\n", on);
}


void aapiAdapterProperties(bt_status_t status, int num_properties, const bt_property_t *properties)
{
    logd("aapiAdapterProperties(...)\n");
}

void aapiRemoteDevProperties(bt_status_t status, const bt_bdaddr_t *bd_addr, int num_properties, const bt_property_t *properties)
{
    logd("aapiRemoteDevProperties(...)\n");
}

void aapiDevDiscoveredCbk(int num_properties, const bt_property_t *properties)
{
    logd("aapiDevDiscoveredCbk(...)\n");
}

void aapiDiscoveryStateChanged(bool on)
{
    logd("aapiDiscoveryStateChanged(%d)\n", on);
}

void aapiPinReqCbk(const struct bt_addr *peer)
{
    const uint8_t pin[4] = {'1','2','3','4'};

    logd("aapiPinReqCbk(...)\n");
    logd("pin is '1234'\n");
    hciSecUserCbkPinCodeReq(peer, pin, sizeof(pin));
}

void aapiSspReqCbk(const struct bt_addr *peer, bt_ssp_variant_t pairing_variant, uint32_t pass_key)
{
    logd("aapiSspReqCbk(...)\n");
    if (pairing_variant == BT_SSP_VARIANT_PASSKEY_ENTRY) {
        logd("entering all zeroes since we do not support number entry for SSP\n");
        hciSecUserCbkSspUserPasskeyReq(peer, 0);
    } else if (pairing_variant == BT_SSP_VARIANT_PASSKEY_CONFIRMATION) {
        logd("confirming display of '%06u'\n", pass_key);
        hciSecUserCbkSspUserConfirmationReq(peer, true);
    } else if (pairing_variant == BT_SSP_VARIANT_PASSKEY_NOTIFICATION) {
        logd("Numeric: '%06u'\n", pass_key);
    }
}

void aapiBondStateChangedCbk(const struct bt_addr *peer, uint8_t state)
{
    logd("aapiBondStateChangedCbk(...)\n");
}

void aapiAclStateChanged(bt_status_t status, const struct bt_addr *peer, bool up)
{
    logd("aapiAclStateChanged(..., %d)\n", up);
}

void aapiDutModeEventCbk(uint16_t opcode, const uint8_t *buf, uint8_t len)
{
    logd("aapiDutModeEventCbk(...)\n");
}

void aapiLeTestModeEventCbk(bt_status_t status, uint16_t num_packets)
{
    logd("aapiLeTestModeEventCbk(...)\n");
}


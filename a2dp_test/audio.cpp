#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "audio.h"
#include "DataPump.h"

#include <tinyalsa/asoundlib.h>
#include <audio_utils/resampler.h>
#include <linux/input.h>

class SpeakerPump : public DataPump {
public:
    SpeakerPump(size_t bufferSize, struct pcm* pcm)
        : DataPump(bufferSize), mPCM(pcm) {}
    virtual ~SpeakerPump() {}

protected:

    virtual int write(void* buffer, int size) {
        pcm_write(mPCM, buffer, size);
        return size;
    }

private:
    struct pcm*   mPCM;
};

struct audio_context {
    struct pcm* pcm;
    int buffer_size;
    struct resampler_itfe *resampler;
    SpeakerPump* speakerPump;
};

#define kTAS5713_VolMin static_cast<float>(0x9e) // -55.0 dB
#define kAndroid_VolMin 0.1f
#define kTAS5713_VolMax static_cast<float>(0x2b) // +2.5 dB
#define kAndroid_VolMax 1.0f
static uint8_t volumeFloatToReg(float vol) {
    if (vol > 1.0) vol = 1.0;
    if (vol < 0.0) vol = 0.0;

    uint8_t ret;
    if (0.0 == vol)
        ret = 0xFF;
    else {
        ret = static_cast<uint8_t>(
                (((vol - kAndroid_VolMin) * (kTAS5713_VolMax - kTAS5713_VolMin))
                                          / (kAndroid_VolMax - kAndroid_VolMin))
                                          + kTAS5713_VolMin + 0.5);
    }

    return ret;
}

#define TAS5713_SET_MASTER_VOLUME _IOW('A', 0xF9, __u8)

static void setMasterVolume(struct pcm *pcm, float vol) {
    uint8_t regVal = volumeFloatToReg(vol);
    int fd = *(reinterpret_cast<int*>(pcm));
    int rc = ::ioctl(fd, TAS5713_SET_MASTER_VOLUME, &regVal);

    if (rc)
        fprintf(stderr, "Tungsten audio hardware unable to set volume, result was %d\n", rc);
}


#define VOL_INCREMENT 0.05

static void* input_thread(void* param) {
    struct pcm* pcm = (struct pcm *)param;
    float volume = 0.5;
    bool muted = false;
    setMasterVolume(pcm, volume);

    const char* device = "/dev/input/event0";
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "could not open %s, %s\n", device, strerror(errno));
        return NULL;
    }

    while (1) {
        struct input_event event;
        if (read(fd, &event, sizeof(event)) == sizeof(event)) {
            fprintf(stderr, "type: %d code: %d value: %d\n", event.type, event.code, event.value);
            if (event.type == EV_KEY && event.value == 1) {
                switch (event.code) {
                    case 113:
                        muted = !muted;
                        break;
                    case 114:
                        if (!muted) {
                            volume -= VOL_INCREMENT;
                            if (volume < 0.0) volume = 0.0;
                        }
                        break;
                    case 115:
                        if (!muted) {
                            volume += VOL_INCREMENT;
                            if (volume > 1.0) volume = 1.0;
                        }
                        break;
                }
                setMasterVolume(pcm, muted ? 0.0 : volume);
            }
        }
    }

    close(fd);
    return NULL;
}


extern "C" struct audio_context* audio_open(unsigned int card, unsigned int device, unsigned int channels,
                 unsigned int in_rate, unsigned int out_rate, unsigned int bits, unsigned int period_size,
                 unsigned int period_count)
{
    struct pcm_config config;
    struct pcm *pcm;
    char *buffer;
    int size;

    config.channels = channels;
    config.rate = out_rate;
    config.period_size = period_size;
    config.period_count = period_count;
    if (bits == 32)
        config.format = PCM_FORMAT_S32_LE;
    else if (bits == 16)
        config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;

    pcm = pcm_open(card, device, PCM_OUT, &config);
    if (!pcm || !pcm_is_ready(pcm)) {
        fprintf(stderr, "Unable to open PCM device %u (%s)\n",
                device, pcm_get_error(pcm));
        return NULL;
    }

    pthread_t tid;
    pthread_create(&tid, NULL, input_thread, pcm);
    setMasterVolume(pcm, 1.0);

    size = pcm_frames_to_bytes(pcm, pcm_get_buffer_size(pcm));
    buffer = (char *)malloc(size);
    if (!buffer) {
        fprintf(stderr, "Unable to allocate %d bytes\n", size);
        free(buffer);
        pcm_close(pcm);
        return NULL;
    }

    struct audio_context* context = (struct audio_context *)malloc(sizeof(struct audio_context));
    context->pcm = pcm;
    context->buffer_size = size;

    if (in_rate == out_rate) {
        context->resampler = NULL;
    } else {
        create_resampler(in_rate, out_rate, 2, RESAMPLER_QUALITY_DEFAULT, NULL, &context->resampler);
        context->resampler->reset(context->resampler);
    }    

    context->speakerPump = new SpeakerPump(10000, context->pcm);
    context->speakerPump->start();

    return context;
}

extern "C" void audio_play(struct audio_context* context, const uint16_t* buffer, int length) {
    SpeakerPump* speakerPump = context->speakerPump;

    uint16_t* destBuffer = (uint16_t *)speakerPump->getEmptyBuffer();

    if (context->resampler) {
        size_t in_frames = length / 2;
        size_t out_frames = speakerPump->getBufferSize() / (2 * sizeof(uint16_t));
        context->resampler->resample_from_input(context->resampler,
                                            (int16_t *)buffer,
                                            &in_frames,
                                            (int16_t *)destBuffer,
                                            &out_frames);
        speakerPump->setBufferCount(destBuffer, out_frames * 4);
    } else {
        memcpy(destBuffer, buffer, length * 2);
        speakerPump->setBufferCount(destBuffer, length * 2);
    }
}

extern "C" void audio_close(struct audio_context* context) {
    // FIXME stop and free speakerPump
    pcm_close(context->pcm);
    free(context);
}


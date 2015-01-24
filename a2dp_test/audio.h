
#ifndef __TINYPLAY
#define __TINYPLAY

#if __cplusplus
extern "C" {
#endif

struct audio_context* audio_open(unsigned int card, unsigned int device, unsigned int channels,
                 unsigned int in_rate, unsigned int out_rate, unsigned int bits, unsigned int period_size,
                 unsigned int period_count);
void audio_play(struct audio_context* context, const uint16_t* buffer, int length);
void audio_close(struct audio_context* context);

#if __cplusplus
}
#endif

#endif // __TINYPLAY

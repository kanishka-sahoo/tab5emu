/*
 * SDL2 audio for the host build, using the queue API. Writes block while
 * more than periods * period_frames are queued, like a DMA queue.
 */
#include <SDL.h>

#include "retro_audio.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_time.h"

#define BYTES_PER_FRAME (2 * sizeof(int16_t))

static SDL_AudioDeviceID s_dev;
static size_t s_queue_max; /* frames */
static int s_volume = 100;
static bool s_mute;

bool retro_audio_open(retro_audio_config_t *cfg)
{
    if (s_dev) {
        return true;
    }
    SDL_AudioSpec want = {0};
    SDL_AudioSpec have;
    want.freq = (int)cfg->sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)cfg->period_frames;

    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_dev == 0) {
        RLOGE(AUDIO, "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }
    /* SDL's own device buffer adds have.samples on top of the queue. */
    s_queue_max = (size_t)cfg->periods * cfg->period_frames;
    RLOGI(AUDIO, "%d Hz stereo, device buffer %u frames, queue %u frames, driver %s", have.freq,
          have.samples, (unsigned)s_queue_max, SDL_GetCurrentAudioDriver());
    SDL_PauseAudioDevice(s_dev, 0);
    return true;
}

void retro_audio_close(void)
{
    if (s_dev) {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
}

size_t retro_audio_write(const int16_t *stereo, size_t frames, uint32_t timeout_ms)
{
    if (!s_dev) {
        return 0;
    }
    uint64_t deadline = retro_time_us() + (uint64_t)timeout_ms * 1000u;
    while (SDL_GetQueuedAudioSize(s_dev) / BYTES_PER_FRAME + frames > s_queue_max) {
        if (timeout_ms != RETRO_WAIT_FOREVER && retro_time_us() >= deadline) {
            return 0;
        }
        retro_sleep_ms(1);
    }
    int16_t tmp[2 * 1024];
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done > 1024 ? 1024 : frames - done;
        for (size_t i = 0; i < 2 * n; i++) {
            tmp[i] = s_mute ? 0 : (int16_t)(stereo[2 * done + i] * s_volume / 100);
        }
        if (SDL_QueueAudio(s_dev, tmp, (Uint32)(n * BYTES_PER_FRAME)) != 0) {
            RLOGW(AUDIO, "SDL_QueueAudio failed: %s", SDL_GetError());
            break;
        }
        done += n;
    }
    return done;
}

bool retro_audio_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    return true;
}

bool retro_audio_set_mute(bool mute)
{
    s_mute = mute;
    return true;
}

retro_audio_route_t retro_audio_route(void)
{
    return RETRO_AUDIO_ROUTE_SPEAKER;
}

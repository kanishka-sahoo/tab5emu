/*
 * SDL2 audio for the host build, using the queue API (no callback).
 */
#include <SDL.h>

#include "retro_audio.h"
#include "retro_log.h"

#define BYTES_PER_FRAME (2 * sizeof(int16_t))

static SDL_AudioDeviceID s_dev;

bool retro_audio_init(unsigned sample_rate)
{
    SDL_AudioSpec want = {0};
    SDL_AudioSpec have;
    want.freq = (int)sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;

    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_dev == 0) {
        RLOGE(AUDIO, "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }
    RLOGI(AUDIO, "%d Hz stereo, %u-frame buffer, driver %s", have.freq, have.samples,
          SDL_GetCurrentAudioDriver());
    SDL_PauseAudioDevice(s_dev, 0);
    return true;
}

void retro_audio_write(const int16_t *stereo, size_t frames)
{
    if (s_dev && SDL_QueueAudio(s_dev, stereo, (Uint32)(frames * BYTES_PER_FRAME)) != 0) {
        RLOGW(AUDIO, "SDL_QueueAudio failed: %s", SDL_GetError());
    }
}

size_t retro_audio_queued_frames(void)
{
    return s_dev ? SDL_GetQueuedAudioSize(s_dev) / BYTES_PER_FRAME : 0;
}

void retro_audio_deinit(void)
{
    if (s_dev) {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
}

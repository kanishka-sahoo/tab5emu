/*
 * RetroHAL audio on the Tab5: ES8388 through the board layer.
 */
#include "retro_audio.h"
#include "retro_tab5.h"

bool retro_audio_open(retro_audio_config_t *cfg)
{
    if (!retro_tab5_audio_open(cfg->sample_rate, cfg->periods, cfg->period_frames)) {
        return false;
    }
    retro_tab5_audio_geometry(&cfg->periods, &cfg->period_frames);
    return true;
}

void retro_audio_close(void)
{
    /* The codec stays open (and shareable, spec §16); just go quiet. */
    retro_tab5_audio_mute(true);
}

size_t retro_audio_write(const int16_t *stereo, size_t frames, uint32_t timeout_ms)
{
    return retro_tab5_audio_write_timeout(stereo, frames, timeout_ms);
}

bool retro_audio_set_volume(int percent)
{
    return retro_tab5_audio_volume(percent);
}

bool retro_audio_set_mute(bool mute)
{
    return retro_tab5_audio_mute(mute);
}

retro_audio_route_t retro_audio_route(void)
{
    /* HP_DET reads 1 with headphones plugged in (bring-up). */
    return retro_tab5_headphones() == 1 ? RETRO_AUDIO_ROUTE_HEADPHONES : RETRO_AUDIO_ROUTE_SPEAKER;
}

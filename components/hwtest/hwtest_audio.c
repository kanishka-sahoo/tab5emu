/*
 * Audio bring-up (plan Phase 1 item 4): ES8388 at 48 kHz stereo, DMA queue
 * depth (output latency), the real I2S rate against esp_timer (what the DRC
 * of plan D3 has to absorb), channel order, speaker amp and headphone
 * detect (R18).
 */
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hwtest_priv.h"
#include "retro_time.h"

#define RATE 48000
#define CHUNK 240 /* frames per write, 5 ms */

static int16_t s_buf[CHUNK * 2];

static void play_tone(float hz_l, float hz_r, int ms)
{
    const float amp = 6000.0f;
    int frames = RATE * ms / 1000;
    float pl = 0, pr = 0;
    const float dl = 2.0f * (float)M_PI * hz_l / RATE, dr = 2.0f * (float)M_PI * hz_r / RATE;
    for (int done = 0; done < frames; done += CHUNK) {
        for (int i = 0; i < CHUNK; i++) {
            s_buf[2 * i] = hz_l > 0 ? (int16_t)(amp * sinf(pl)) : 0;
            s_buf[2 * i + 1] = hz_r > 0 ? (int16_t)(amp * sinf(pr)) : 0;
            pl = fmodf(pl + dl, 2.0f * (float)M_PI);
            pr = fmodf(pr + dr, 2.0f * (float)M_PI);
        }
        retro_tab5_audio_write(s_buf, CHUNK);
    }
}

static void play_silence(int ms)
{
    memset(s_buf, 0, sizeof(s_buf));
    for (int done = 0; done < RATE * ms / 1000; done += CHUNK) {
        retro_tab5_audio_write(s_buf, CHUNK);
    }
}

void hwtest_audio(void)
{
    if (!retro_tab5_audio_init(RATE)) {
        hw_result(HW_FAIL, "audio.init", "ES8388 / I2S init failed");
        return;
    }
    retro_tab5_audio_volume(60);
    hw_result(HW_PASS, "audio.init", "ES8388 %u Hz stereo s16", RATE);

    /* Queue depth: silence goes in without blocking until the DMA
     * descriptors are full. Settle first so the start state is "full". */
    play_silence(100);
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the queue drain */
    memset(s_buf, 0, sizeof(s_buf));
    size_t accepted = 0;
    for (;;) {
        uint64_t t0 = retro_time_us();
        retro_tab5_audio_write(s_buf, CHUNK);
        if (retro_time_us() - t0 > 2000 || accepted > RATE) {
            break;
        }
        accepted += CHUNK;
    }
    size_t cfg = retro_tab5_audio_dma_frames();
    hw_result(HW_INFO, "audio.dma_queue", "%u frames (%.1f ms) accepted before blocking; config %u (%.1f ms)",
              (unsigned)accepted, accepted * 1000.0 / RATE, (unsigned)cfg, cfg * 1000.0 / RATE);

    /* With the queue full, writes pace at the real I2S rate. */
    size_t frames = 0;
    uint64_t t0 = retro_time_us();
    while (frames < 3 * RATE) {
        retro_tab5_audio_write(s_buf, CHUNK);
        frames += CHUNK;
    }
    double rate = (double)frames * 1e6 / (double)(retro_time_us() - t0);
    hw_result(HW_INFO, "audio.i2s_rate_hz", "%.1f over 3 s (%+.0f ppm vs esp_timer)", rate,
              (rate / RATE - 1.0) * 1e6);

    RLOGI(AUDIO, "listen: left 440 Hz, right 660 Hz, both 880 Hz, then 880 Hz with the amp off");
    play_tone(440, 0, 700);
    play_silence(150);
    play_tone(0, 660, 700);
    play_silence(150);
    play_tone(880, 880, 700);
    play_silence(150);
    hw_result(HW_INFO, "audio.tones", "played L 440 / R 660 / both 880 Hz: confirm by ear");

    retro_tab5_rail_set(RETRO_TAB5_RAIL_SPEAKER, false);
    play_tone(880, 880, 700);
    play_silence(100);
    retro_tab5_rail_set(RETRO_TAB5_RAIL_SPEAKER, true);
    hw_result(HW_INFO, "audio.amp_off", "last tone had SPK_EN low: silent on the speaker?");

    int hp = retro_tab5_headphones();
    hw_result(hp < 0 ? HW_FAIL : HW_INFO, "audio.hp_det", "HP_DET reads %d now (run \"hp\" and plug in)",
              hp);
}

void hwtest_headphones_watch(int seconds)
{
    int last = retro_tab5_headphones();
    int changes = 0;
    bool seen[2] = {last == 0, last == 1};
    RLOGI(AUDIO, "watching HP_DET for %d s: plug and unplug headphones (now %d)", seconds, last);
    uint32_t end = retro_time_ms() + (uint32_t)seconds * 1000;
    while ((int32_t)(end - retro_time_ms()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int v = retro_tab5_headphones();
        if (v >= 0 && v != last) {
            RLOGI(AUDIO, "HP_DET %d -> %d", last, v);
            last = v;
            seen[v] = true;
            changes++;
        }
    }
    hw_result(changes ? HW_PASS : HW_WARN, "audio.hp_det_watch", "%d changes, levels seen:%s%s",
              changes, seen[0] ? " 0" : "", seen[1] ? " 1" : "");
}

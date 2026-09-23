/*
 * Frontend bring-up and the emulator frame loop (see emulator_manager.h).
 */
#include <math.h>
#include <string.h>

#include "audio_engine.h"
#include "controller_manager.h"
#include "emu_config.h"
#include "emulator_manager.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_power.h"
#include "retro_storage.h"
#include "retro_time.h"
#include "video_pipeline.h"

#define CONTROLLERS_JSON RETRO_STORAGE_SD "/retro/config/controllers.json"

static struct {
    emu_config_t cfg;
    volatile bool running;
    retro_task_t *task;
    retro_mutex_t *lock;
    retro_av_info_t av;
    int16_t *audio;
    size_t audio_cap;

    uint32_t frames;
    uint64_t run_us_sum;
    uint32_t run_n, run_us_max;
} s;

bool emu_frontend_start(unsigned max_w, unsigned max_h)
{
    const audio_engine_config_t ac = {
        .out_rate = CONFIG_RETRO_AUDIO_RATE,
        .period_frames = CONFIG_RETRO_AUDIO_PERIOD_FRAMES,
        .periods = CONFIG_RETRO_AUDIO_PERIODS,
        .latency_ms = CONFIG_RETRO_AUDIO_LATENCY_MS,
        .core = CONFIG_RETRO_TASK_AUDIO_CORE,
        .priority = CONFIG_RETRO_TASK_AUDIO_PRIO,
    };
    if (!audio_engine_start(&ac)) {
        RLOGE(AUDIO, "audio engine failed; continuing without sound");
    }
    const vp_config_t vc = {
        .max_width = max_w,
        .max_height = max_h,
        .core = CONFIG_RETRO_TASK_VIDEO_CORE,
        .priority = CONFIG_RETRO_TASK_VIDEO_PRIO,
    };
    if (!vp_start(&vc)) {
        return false;
    }
    cm_init();
    if (retro_storage_mounted("sd")) {
        cm_mapping_load(CONTROLLERS_JSON);
    }
    cm_keys_source_start();
    cm_touch_source_start(CONFIG_RETRO_TASK_INPUT_CORE, CONFIG_RETRO_TASK_INPUT_PRIO);
#if CONFIG_RETRO_USB_PADS
    cm_xinput_start(CONFIG_RETRO_TASK_INPUT_CORE, CONFIG_RETRO_TASK_USB_PRIO);
#else
    /* USB pads are deferred to v2 (plan D11, D14); v1 input is touch only,
     * so USB-A 5 V stays off. */
    retro_power_rail_set(RETRO_RAIL_USB_HOST, false);
#endif
    return true;
}

void emu_frontend_stop(void)
{
    emu_stop();
    vp_stop();
    audio_engine_stop();
}

static void emu_task(void *arg)
{
    (void)arg;
    const retro_core_t *core = s.cfg.core;
    uint32_t prev_hot = 0;
    /* Normally the audio ring paces this loop (plan D3). Without audio
     * (the engine failed to start) pace on the clock instead. */
    const uint64_t frame_us = (uint64_t)(1e6 / (s.av.fps > 1.0 ? s.av.fps : 60.0));
    uint64_t next_us = retro_time_us();
    while (s.running) {
        retro_input_state_t in;
        cm_poll(&in);
        uint32_t pressed = in.hotkeys & ~prev_hot;
        prev_hot = in.hotkeys;
        if (pressed && s.cfg.on_hotkey) {
            s.cfg.on_hotkey(pressed, s.cfg.hotkey_ctx);
        }
        core->input(&in);

        retro_video_frame_t vf;
        vp_frame_begin(&vf);
        retro_audio_frame_t af = {s.audio, s.audio_cap, 0};
        uint64_t t0 = retro_time_us();
        core->run_frame(&vf, &af);
        uint32_t us = (uint32_t)(retro_time_us() - t0);

        /* Video first: publishing never blocks, and the frame is then
         * ready while we wait on audio. */
        vp_frame_publish(vf.width, vf.height);
        if (audio_engine_push(af.samples, af.frames) < af.frames || af.frames == 0) {
            next_us += frame_us;
            uint64_t now = retro_time_us();
            if (next_us > now) {
                retro_sleep_us((uint32_t)(next_us - now));
            } else if (now - next_us > 4 * frame_us) {
                next_us = now; /* fell far behind: don't try to catch up */
            }
        } else {
            next_us = retro_time_us();
        }

        retro_mutex_lock(s.lock);
        s.frames++;
        s.run_us_sum += us;
        s.run_n++;
        if (us > s.run_us_max) {
            s.run_us_max = us;
        }
        retro_mutex_unlock(s.lock);
    }
}

bool emu_start(const emu_config_t *cfg)
{
    if (s.running) {
        return false;
    }
    s.cfg = *cfg;
    const retro_core_t *core = cfg->core;
    if (core->load(cfg->content) != 0) {
        RLOGE(CORE, "%s: load failed", core->name);
        return false;
    }
    core->get_av_info(&s.av);
    /* Room for a slow frame's worth of samples plus slack. */
    s.audio_cap = (size_t)ceil(s.av.sample_rate / (s.av.fps > 1.0 ? s.av.fps : 1.0)) * 2 + 64;
    s.audio = retro_mem_alloc(s.audio_cap * 2 * sizeof(int16_t), RETRO_MEM_INTERNAL | RETRO_MEM_FALLBACK);
    if (!s.lock) {
        s.lock = retro_mutex_create();
    }
    if (!s.audio || !s.lock) {
        retro_mem_free(s.audio);
        s.audio = NULL;
        core->unload();
        return false;
    }
    audio_engine_set_input_rate(s.av.sample_rate);
    s.frames = 0;
    s.running = true;
    const retro_task_config_t tc = {
        .name = "emu",
        .fn = emu_task,
        .stack_bytes = 16384,
        .priority = CONFIG_RETRO_TASK_EMU_PRIO,
        .core = CONFIG_RETRO_TASK_EMU_CORE,
    };
    s.task = retro_task_create(&tc);
    if (!s.task) {
        s.running = false;
        retro_mem_free(s.audio);
        s.audio = NULL;
        core->unload();
        return false;
    }
    RLOGI(CORE, "%s running: %ux%u @ %.4f Hz, audio %u Hz", core->name, s.av.base_width,
          s.av.base_height, s.av.fps, s.av.sample_rate);
    return true;
}

void emu_stop(void)
{
    if (!s.running) {
        return;
    }
    s.running = false;
    /* The emu task may be blocked in audio_engine_push(); the audio task
     * frees ring space every period, so it returns within a few ms. */
    retro_task_join(s.task);
    s.task = NULL;
    s.cfg.core->unload();
    retro_mem_free(s.audio);
    s.audio = NULL;
}

bool emu_running(void)
{
    return s.running;
}

uint32_t emu_frame_count(void)
{
    if (!s.lock) {
        return 0;
    }
    retro_mutex_lock(s.lock);
    uint32_t f = s.frames;
    retro_mutex_unlock(s.lock);
    return f;
}

void emu_get_stats(emu_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s.lock) {
        return;
    }
    retro_mutex_lock(s.lock);
    out->frames = s.frames;
    out->run_ms_avg = s.run_n ? (float)s.run_us_sum / (float)s.run_n / 1000.0f : 0.0f;
    out->run_ms_max = (float)s.run_us_max / 1000.0f;
    out->fps_target = s.av.fps;
    s.run_us_sum = 0;
    s.run_n = 0;
    s.run_us_max = 0;
    retro_mutex_unlock(s.lock);
}

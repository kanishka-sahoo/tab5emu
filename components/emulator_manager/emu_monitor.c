/*
 * Statistics task: performance overlay (spec §45) and periodic log summary.
 */
#include <stdio.h>
#include <string.h>

#include "audio_engine.h"
#include "controller_manager.h"
#include "emu_config.h"
#include "emulator_manager.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_power.h"
#include "retro_time.h"
#include "video_pipeline.h"

#define PERIOD_MS 500
#define POWER_EVERY 4 /* periods: INA226 read every 2 s */

typedef struct {
    uint64_t t_us;
    uint32_t emu_frames, rendered, vsyncs, dropped, underruns;
} snap_t;

static struct {
    bool started;
    volatile bool stop;
    retro_task_t *task;
    volatile bool overlay;
    unsigned log_every_s;
    retro_mutex_t *lock;
    char line[256]; /* last summary, for emu_monitor_log() */

    /* Extremes over the log window. */
    float run_max, render_max;
    int32_t ppm_min, ppm_max;
} s;

static void take_snap(snap_t *o)
{
    emu_stats_t e;
    vp_stats_t v;
    audio_engine_stats_t a;
    /* Baseline counters. Also resets the averages, which is fine at start. */
    emu_get_stats(&e);
    vp_get_stats(&v);
    audio_engine_get_stats(&a);
    *o = (snap_t){retro_time_us(), e.frames, v.rendered, v.vsyncs, v.dropped, a.underruns};
}

static float rate(uint32_t a, uint32_t b, uint64_t dt_us)
{
    return dt_us ? (float)(b - a) * 1e6f / (float)dt_us : 0.0f;
}

static void tick(snap_t *prev, retro_power_status_t *pwr, bool read_power)
{
    emu_stats_t e;
    vp_stats_t v;
    audio_engine_stats_t a;
    cm_stats_t c;
    emu_get_stats(&e);
    vp_get_stats(&v);
    audio_engine_get_stats(&a);
    cm_get_stats(&c);
    int cpu[RETRO_MAX_CPUS];
    retro_cpu_load(cpu);
    retro_heap_info_t heap;
    retro_heap_info(&heap);
    if (read_power) {
        retro_power_status(pwr);
    }

    const snap_t now = {retro_time_us(), e.frames, v.rendered, v.vsyncs, v.dropped, a.underruns};
    const uint64_t dt = now.t_us - prev->t_us;
    const float emu_fps = rate(prev->emu_frames, now.emu_frames, dt);
    const float out_fps = rate(prev->rendered, now.rendered, dt);
    const float hz = rate(prev->vsyncs, now.vsyncs, dt);
    *prev = now;

    retro_mutex_lock(s.lock);
    s.run_max = e.run_ms_max > s.run_max ? e.run_ms_max : s.run_max;
    s.render_max = v.render_ms_max > s.render_max ? v.render_ms_max : s.render_max;
    s.ppm_min = a.drc_ppm < s.ppm_min ? a.drc_ppm : s.ppm_min;
    s.ppm_max = a.drc_ppm > s.ppm_max ? a.drc_ppm : s.ppm_max;
    retro_mutex_unlock(s.lock);

    const float fill_ms = a.in_rate ? (float)a.fill_frames * 1000.0f / (float)a.in_rate : 0.0f;
    if (s.overlay) {
        char t[768];
        int n = 0;
        n += snprintf(t + n, sizeof(t) - (size_t)n, "EMU %.2f fps  OUT %.2f\n", (double)emu_fps,
                      (double)out_fps);
        n += snprintf(t + n, sizeof(t) - (size_t)n, "LCD %.2f Hz  drop %u\n", (double)hz,
                      (unsigned)v.dropped);
        n += snprintf(t + n, sizeof(t) - (size_t)n, "frame %.1f ms (max %.1f)\n",
                      (double)e.run_ms_avg, (double)e.run_ms_max);
        n += snprintf(t + n, sizeof(t) - (size_t)n, "blit %.1f ms %s\n", (double)v.render_ms_avg,
                      v.path ? v.path : "-");
        if (cpu[0] >= 0) {
            n += snprintf(t + n, sizeof(t) - (size_t)n, "CPU0 %d%%  CPU1 %d%%\n", cpu[0], cpu[1]);
        }
        n += snprintf(t + n, sizeof(t) - (size_t)n, "SRAM %uK (min %uK)\n",
                      (unsigned)(heap.internal_free >> 10), (unsigned)(heap.internal_min_free >> 10));
        n += snprintf(t + n, sizeof(t) - (size_t)n, "PSRAM %uK free\n",
                      (unsigned)(heap.psram_free >> 10));
        n += snprintf(t + n, sizeof(t) - (size_t)n, "audio %.1f ms  xrun %u\n", (double)fill_ms,
                      (unsigned)a.underruns);
        n += snprintf(t + n, sizeof(t) - (size_t)n, "DRC %+d ppm  wait %.0f ms\n", (int)a.drc_ppm,
                      (double)a.push_wait_ms);
        n += snprintf(t + n, sizeof(t) - (size_t)n, "pads %d  %s\n", c.pads,
                      c.pads ? c.names[0] : "(touch)");
        if (pwr->valid && pwr->battery_present) {
            n += snprintf(t + n, sizeof(t) - (size_t)n, "batt %.2fV %dmA %d%%\n",
                          (double)pwr->battery_mv / 1000.0, (int)pwr->current_ma, pwr->percent);
        } else if (pwr->valid) {
            n += snprintf(t + n, sizeof(t) - (size_t)n, "on external power\n");
        }
        vp_overlay_set_text(t);
    }

    retro_mutex_lock(s.lock);
    snprintf(s.line, sizeof(s.line),
             "emu %.2f fps, out %.2f fps, lcd %.2f Hz | frame max %.1f ms, blit %.1f/%.1f ms %s | "
             "audio %.1f ms, xrun %u, drc %+d..%+d ppm | dropped %u | CPU %d/%d%% | SRAM %uK | pads %d",
             (double)emu_fps, (double)out_fps, (double)hz, (double)s.run_max,
             (double)v.render_ms_avg, (double)s.render_max, v.path ? v.path : "-", (double)fill_ms,
             (unsigned)a.underruns, (int)s.ppm_min, (int)s.ppm_max, (unsigned)v.dropped, cpu[0],
             cpu[1], (unsigned)(heap.internal_free >> 10), c.pads);
    retro_mutex_unlock(s.lock);
}

void emu_monitor_log(void)
{
    if (!s.lock) {
        return;
    }
    retro_mutex_lock(s.lock);
    RLOGI(CORE, "%s", s.line);
    s.run_max = s.render_max = 0.0f;
    s.ppm_min = s.ppm_max = 0;
    retro_mutex_unlock(s.lock);
}

static void monitor_task(void *arg)
{
    (void)arg;
    snap_t prev;
    take_snap(&prev);
    retro_power_status_t pwr = {0};
    unsigned n = 0;
    uint64_t next_log = retro_time_us() + (uint64_t)s.log_every_s * 1000000u;
    bool overlay_was = false;
    while (!s.stop) {
        retro_sleep_ms(PERIOD_MS);
        tick(&prev, &pwr, n++ % POWER_EVERY == 0);
        if (overlay_was && !s.overlay) {
            vp_overlay_set_text(NULL);
        }
        overlay_was = s.overlay;
        if (s.log_every_s && retro_time_us() >= next_log) {
            next_log += (uint64_t)s.log_every_s * 1000000u;
            emu_monitor_log();
        }
    }
}

bool emu_monitor_start(bool overlay, unsigned log_every_s)
{
    s.overlay = overlay;
    s.log_every_s = log_every_s;
    if (s.started) {
        return true;
    }
    s.lock = retro_mutex_create();
    retro_power_init();
    const retro_task_config_t tc = {
        .name = "monitor",
        .fn = monitor_task,
        .stack_bytes = 6144,
        .priority = CONFIG_RETRO_TASK_MONITOR_PRIO,
        .core = 1,
    };
    s.stop = false;
    s.task = s.lock ? retro_task_create(&tc) : NULL;
    s.started = s.task != NULL;
    return s.started;
}

void emu_monitor_stop(void)
{
    if (s.started) {
        s.stop = true;
        retro_task_join(s.task);
        s.task = NULL;
        s.started = false;
    }
}

void emu_monitor_set_overlay(bool on)
{
    s.overlay = on;
}

bool emu_monitor_overlay(void)
{
    return s.overlay;
}

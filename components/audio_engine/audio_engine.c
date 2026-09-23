/*
 * Audio engine: SPSC ring + DRC resampler + audio_out feeder task (see
 * audio_engine.h).
 */
#include "audio_engine.h"

#include <stdatomic.h>
#include <string.h>

#include "ae_resampler.h"
#include "retro_audio.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_spsc.h"
#include "retro_time.h"

/* Power of two. 85 ms at 48 kHz, 128 ms at 32 kHz: well above any
 * high-water mark we'd configure. */
#define RING_FRAMES 4096
#define MAX_PPM 5000       /* +/-0.5 % (plan D3) */
#define IN_CHUNK 256       /* frames moved from the ring per resampler call */
#define MAX_PERIOD 1024
#define FILL_EMA_SHIFT 4   /* 1/16 per period: ~80 ms time constant at 5 ms */
#define KEEPING_UP_PERIODS 20 /* producer blocked within this many periods */

static uint32_t s_ring_mem[RING_FRAMES];

static struct {
    audio_engine_config_t cfg;
    _Atomic bool running;
    retro_task_t *task;
    retro_spsc_t ring;
    retro_sem_t *space;

    _Atomic unsigned in_rate;
    _Atomic bool rate_changed;
    _Atomic uint32_t hwm;       /* native frames */
    _Atomic uint32_t last_push; /* frames in the latest push */
    _Atomic bool paused;

    audio_mix_fn mix;
    void *mix_ctx;

    _Atomic uint32_t underruns;
    _Atomic int32_t drc_ppm;
    _Atomic uint64_t out_frames, in_frames;
    _Atomic uint64_t push_wait_us;
    _Atomic uint32_t push_waits; /* times the producer blocked */
} s;

/* Average fill the producer settles at when it outruns the output: it blocks
 * at the high-water mark, then adds a whole push at once. */
static uint32_t fill_target(void)
{
    uint32_t hwm = atomic_load(&s.hwm);
    uint32_t n = atomic_load(&s.last_push);
    uint32_t top = hwm > n ? hwm : n;
    return top - n / 2;
}

static void audio_task(void *arg)
{
    (void)arg;
    const size_t period = s.cfg.period_frames;
    static int16_t out[2 * MAX_PERIOD];
    static uint32_t in_buf[IN_CHUNK];
    size_t in_len = 0, in_off = 0;
    ae_resampler_t rs;
    ae_resampler_init(&rs, atomic_load(&s.in_rate), s.cfg.out_rate);
    bool starving = true; /* wait for the ring to fill before playing */
    uint32_t fill_ema = 0;
    uint32_t last_waits = 0;
    unsigned since_wait = KEEPING_UP_PERIODS;

    while (atomic_load(&s.running)) {
        if (atomic_exchange(&s.rate_changed, false)) {
            ae_resampler_init(&rs, atomic_load(&s.in_rate), s.cfg.out_rate);
            retro_spsc_clear(&s.ring);
            in_len = in_off = 0;
            starving = true;
        }
        const uint32_t target = fill_target();
        uint32_t fill = (uint32_t)(retro_spsc_used(&s.ring) + (in_len - in_off));
        size_t produced = 0;

        if (atomic_load(&s.paused)) {
            starving = true;
        } else if (starving && fill >= target / 2 && fill > 0) {
            starving = false;
            fill_ema = fill;
        }
        if (!starving) {
            while (produced < period) {
                if (in_off == in_len) {
                    in_len = retro_spsc_read(&s.ring, in_buf, IN_CHUNK);
                    in_off = 0;
                    if (in_len == 0) {
                        break;
                    }
                }
                size_t used;
                produced += ae_resample(&rs, (const int16_t *)(in_buf + in_off), in_len - in_off,
                                        &used, out + 2 * produced, period - produced);
                in_off += used;
            }
            if (produced < period) {
                atomic_fetch_add(&s.underruns, 1);
                starving = true;
            }
        }
        if (produced < period) {
            memset(out + 2 * produced, 0, (period - produced) * 2 * sizeof(int16_t));
        }

        /* DRC on the smoothed fill. A producer that has had to block
         * recently is keeping up: the fill then only dips by the time it
         * takes to make a frame, which isn't a deficit, so don't slow down
         * for it. */
        fill_ema += (int32_t)(fill - fill_ema) >> FILL_EMA_SHIFT;
        const uint32_t waits = atomic_load(&s.push_waits);
        since_wait = waits != last_waits ? 0 : since_wait + (since_wait < KEEPING_UP_PERIODS);
        last_waits = waits;
        int32_t ppm = starving ? 0 : ae_drc_ppm(fill_ema, target, MAX_PPM);
        if (ppm < 0 && since_wait < KEEPING_UP_PERIODS) {
            ppm = 0;
        }
        ae_resampler_set_ppm(&rs, ppm);
        atomic_store(&s.drc_ppm, ppm);

        if (s.mix) {
            s.mix(out, period, s.mix_ctx);
        }
        size_t done = 0;
        while (done < period && atomic_load(&s.running)) {
            done += retro_audio_write(out + 2 * done, period - done, 100);
        }
        atomic_fetch_add(&s.out_frames, period);
        retro_sem_give(s.space);
    }
}

bool audio_engine_start(const audio_engine_config_t *cfg)
{
    if (atomic_load(&s.running)) {
        return true;
    }
    s.cfg = *cfg;
    if (s.cfg.period_frames > MAX_PERIOD) {
        s.cfg.period_frames = MAX_PERIOD;
    }
    retro_audio_config_t ac = {cfg->out_rate, s.cfg.period_frames, cfg->periods};
    if (!retro_audio_open(&ac)) {
        return false;
    }
    s.cfg.out_rate = ac.sample_rate;
    s.cfg.period_frames = ac.period_frames;
    s.cfg.periods = ac.periods;

    retro_spsc_init(&s.ring, s_ring_mem, RING_FRAMES);
    s.space = retro_sem_create(1, 0);
    atomic_store(&s.in_rate, s.cfg.out_rate);
    atomic_store(&s.hwm, s.cfg.out_rate * s.cfg.latency_ms / 1000);
    atomic_store(&s.last_push, 0);
    atomic_store(&s.rate_changed, false);
    atomic_store(&s.underruns, 0);
    atomic_store(&s.running, true);

    const retro_task_config_t tc = {
        .name = "audio_out",
        .fn = audio_task,
        .stack_bytes = 4096,
        .priority = cfg->priority,
        .core = cfg->core,
    };
    s.task = retro_task_create(&tc);
    if (!s.task) {
        atomic_store(&s.running, false);
        retro_audio_close();
        return false;
    }
    RLOGI(AUDIO, "engine up: %u Hz out, DMA %u x %u frames (%.1f ms), ring HWM %u ms",
          s.cfg.out_rate, s.cfg.periods, s.cfg.period_frames,
          (double)(s.cfg.periods * s.cfg.period_frames) * 1000.0 / s.cfg.out_rate,
          s.cfg.latency_ms);
    return true;
}

void audio_engine_stop(void)
{
    if (!atomic_exchange(&s.running, false)) {
        return;
    }
    retro_sem_give(s.space); /* release a blocked producer */
    retro_task_join(s.task);
    s.task = NULL;
    retro_audio_close();
    retro_sem_destroy(s.space);
    s.space = NULL;
}

void audio_engine_set_input_rate(unsigned hz)
{
    if (hz == 0 || hz == atomic_load(&s.in_rate)) {
        return;
    }
    atomic_store(&s.in_rate, hz);
    atomic_store(&s.hwm, hz * s.cfg.latency_ms / 1000);
    /* The consumer (which owns the read side) drops the queued samples and
     * resets its resampler when it sees the flag. */
    atomic_store(&s.rate_changed, true);
    RLOGI(AUDIO, "input %u Hz -> %u Hz", hz, s.cfg.out_rate);
}

size_t audio_engine_push(const int16_t *stereo, size_t frames)
{
    size_t done = 0;
    uint64_t waited = 0;
    atomic_store(&s.last_push, (uint32_t)frames);
    while (done < frames && atomic_load(&s.running)) {
        size_t n = frames - done;
        if (n > RING_FRAMES / 2) {
            n = RING_FRAMES / 2;
        }
        const uint32_t hwm = atomic_load(&s.hwm);
        size_t limit = hwm > n ? hwm : n;
        if (limit > RING_FRAMES) {
            limit = RING_FRAMES; /* a long latency setting at a high rate */
        }
        size_t used = retro_spsc_used(&s.ring);
        if (used + n > limit && used > 0) {
            atomic_fetch_add(&s.push_waits, 1);
            uint64_t t0 = retro_time_us();
            retro_sem_take(s.space, 50);
            waited += retro_time_us() - t0;
            continue;
        }
        done += retro_spsc_write(&s.ring, (const uint32_t *)(const void *)(stereo + 2 * done), n);
    }
    atomic_fetch_add(&s.in_frames, done);
    atomic_fetch_add(&s.push_wait_us, waited);
    return done;
}

void audio_engine_set_paused(bool paused)
{
    atomic_store(&s.paused, paused);
}

void audio_engine_set_mix_hook(audio_mix_fn fn, void *ctx)
{
    s.mix_ctx = ctx;
    s.mix = fn;
}

void audio_engine_get_stats(audio_engine_stats_t *out)
{
    out->underruns = atomic_load(&s.underruns);
    out->fill_frames = atomic_load(&s.running) ? (uint32_t)retro_spsc_used(&s.ring) : 0;
    out->hwm_frames = atomic_load(&s.hwm);
    out->drc_ppm = atomic_load(&s.drc_ppm);
    out->out_frames = atomic_load(&s.out_frames);
    out->in_frames = atomic_load(&s.in_frames);
    out->push_wait_ms = (float)atomic_exchange(&s.push_wait_us, 0) / 1000.0f;
    out->in_rate = atomic_load(&s.in_rate);
    out->out_rate = s.cfg.out_rate;
}

#include <math.h>
#include <stdlib.h>

#include "ae_resampler.h"
#include "test_util.h"

/* Deterministic full-scale noise (xorshift). */
static uint32_t s_rng = 0x12345678u;

static int16_t noise(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (int16_t)(s_rng & 0xFFFF);
}

static void fill_noise(int16_t *buf, size_t frames)
{
    for (size_t i = 0; i < 2 * frames; i++) {
        buf[i] = noise();
    }
}

/* Feed in in chunks of in_chunk frames, draining at most out_chunk frames
 * per call. Returns frames produced. */
static size_t run_chunked(ae_resampler_t *r, const int16_t *in, size_t in_frames,
                          size_t in_chunk, int16_t *out, size_t out_cap, size_t out_chunk)
{
    size_t done_in = 0, done_out = 0;
    while (done_in < in_frames && done_out < out_cap) {
        size_t n_in = in_frames - done_in < in_chunk ? in_frames - done_in : in_chunk;
        size_t n_out = out_cap - done_out < out_chunk ? out_cap - done_out : out_chunk;
        size_t used;
        done_out += ae_resample(r, in + 2 * done_in, n_in, &used, out + 2 * done_out, n_out);
        CHECK(used <= n_in);
        done_in += used;
    }
    return done_out;
}

static void test_passthrough_equal_rates(void)
{
    enum { N = 1000 };
    static int16_t in[2 * N], out[2 * N];
    fill_noise(in, N);
    ae_resampler_t r;
    ae_resampler_init(&r, 48000, 48000);
    size_t used;
    size_t n = ae_resample(&r, in, N, &used, out, N);
    /* One frame of lookahead: the last input waits for its successor. */
    CHECK(n == N - 1);
    CHECK(used == N);
    CHECK(memcmp(in, out, (N - 1) * 2 * sizeof(int16_t)) == 0);
    /* It comes out with the next call. */
    int16_t more[2] = {1, 2}, one[2];
    CHECK(ae_resample(&r, more, 1, &used, one, 1) == 1);
    CHECK(one[0] == in[2 * (N - 1)] && one[1] == in[2 * (N - 1) + 1]);
}

static void test_empty_input(void)
{
    ae_resampler_t r;
    ae_resampler_init(&r, 44100, 48000);
    int16_t out[4];
    size_t used = 99;
    CHECK(ae_resample(&r, NULL, 0, &used, out, 2) == 0);
    CHECK(used == 0);
}

static void test_chunked_equals_one_shot(void)
{
    enum { N = 4410, OUT = 6000 };
    static int16_t in[2 * N], a[2 * OUT], b[2 * OUT];
    fill_noise(in, N);
    ae_resampler_t r;
    ae_resampler_init(&r, 44100, 48000);
    size_t used;
    size_t na = ae_resample(&r, in, N, &used, a, OUT);
    CHECK(used == N);
    static const size_t chunks[][2] = {{1, 1}, {7, 3}, {100, 1000}, {1000, 17}, {N, 1}};
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        memset(b, 0, sizeof(b));
        ae_resampler_init(&r, 44100, 48000);
        size_t nb = run_chunked(&r, in, N, chunks[c][0], b, OUT, chunks[c][1]);
        CHECK(nb == na);
        CHECK(memcmp(a, b, na * 2 * sizeof(int16_t)) == 0);
    }
}

static void test_upsample_ratio(void)
{
    enum { N = 44100, OUT = 50000 };
    static int16_t in[2 * N], out[2 * OUT];
    fill_noise(in, N);
    ae_resampler_t r;
    ae_resampler_init(&r, 44100, 48000);
    size_t used;
    size_t n = ae_resample(&r, in, N, &used, out, OUT);
    CHECK(used == N);
    CHECK(n >= 47998 && n <= 48000);
}

static void test_downsample_ratio(void)
{
    enum { N = 48000, OUT = 50000 };
    static int16_t in[2 * N], out[2 * OUT];
    fill_noise(in, N);
    ae_resampler_t r;
    ae_resampler_init(&r, 48000, 32000);
    size_t used;
    size_t n = ae_resample(&r, in, N, &used, out, OUT);
    CHECK(n >= 31998 && n <= 32000);
}

/* Linear interpolation, computed independently: output n sits at input
 * position n * step (Q32) from the first frame. */
static void test_interpolation_exact(void)
{
    enum { N = 2000, OUT = 2400 };
    static int16_t in[2 * N], out[2 * OUT];
    fill_noise(in, N); /* full scale: exercises the widest (b - a) * f */
    ae_resampler_t r;
    ae_resampler_init(&r, 44100, 48000);
    size_t used;
    size_t n = ae_resample(&r, in, N, &used, out, OUT);
    int bad = 0;
    for (size_t k = 0; k < n; k++) {
        uint64_t pos = (uint64_t)k * r.base_step;
        size_t i = (size_t)(pos >> 32);
        int64_t f = (int64_t)((pos & (AE_ONE - 1)) >> 16);
        for (int c = 0; c < 2; c++) {
            int64_t a = in[2 * i + (size_t)c], b = in[2 * (i + 1) + (size_t)c];
            int64_t want = a + (int64_t)floor((double)((b - a) * f) / 65536.0);
            bad += out[2 * k + (size_t)c] != want;
        }
    }
    CHECK(bad == 0);
}

static int zero_crossings(const int16_t *buf, size_t frames)
{
    int n = 0;
    for (size_t i = 1; i < frames; i++) {
        n += (buf[2 * (i - 1)] < 0) != (buf[2 * i] < 0);
    }
    return n;
}

static void test_sine_frequency_preserved(void)
{
    /* One second of 1 kHz at 44.1 kHz becomes one second at 48 kHz: the
     * same 2000 zero crossings. */
    enum { N = 44100, OUT = 50000 };
    static int16_t in[2 * N], out[2 * OUT];
    const double pi = 3.14159265358979323846;
    for (size_t i = 0; i < N; i++) {
        /* A small phase offset keeps samples off exact zero. */
        int16_t v = (int16_t)lrint(20000.0 * sin(2 * pi * 1000.0 * ((double)i + 0.25) / 44100.0));
        in[2 * i] = in[2 * i + 1] = v;
    }
    ae_resampler_t r;
    ae_resampler_init(&r, 44100, 48000);
    size_t used;
    size_t n = ae_resample(&r, in, N, &used, out, OUT);
    int zin = zero_crossings(in, N), zout = zero_crossings(out, n);
    CHECK(abs(zin - 2000) <= 2);
    CHECK(abs(zout - zin) <= 2);
    /* And the amplitude survives. */
    int16_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        peak = out[2 * i] > peak ? out[2 * i] : peak;
    }
    CHECK(peak > 19500 && peak <= 20000);
}

static size_t consumed_for(int32_t ppm, size_t outputs)
{
    enum { N = 120000 };
    static int16_t in[2 * N], out[2 * N];
    ae_resampler_t r;
    ae_resampler_init(&r, 48000, 48000);
    ae_resampler_set_ppm(&r, ppm);
    size_t used;
    size_t n = ae_resample(&r, in, N, &used, out, outputs);
    CHECK(n == outputs);
    return used;
}

static void test_ppm_changes_consumption(void)
{
    /* 100000 outputs at +/-1000 ppm consume 100 frames more/less. */
    size_t base = consumed_for(0, 100000);
    size_t fast = consumed_for(1000, 100000);
    size_t slow = consumed_for(-1000, 100000);
    CHECK(base == 100000);
    CHECK(fast >= base + 99 && fast <= base + 101);
    CHECK(slow + 99 <= base && slow + 101 >= base);
    /* Setting 0 ppm restores the base step. */
    ae_resampler_t r;
    ae_resampler_init(&r, 44100, 48000);
    uint64_t step = r.step;
    ae_resampler_set_ppm(&r, 500);
    CHECK(r.step > step);
    ae_resampler_set_ppm(&r, 0);
    CHECK(r.step == step);
}

static void test_drc_ppm(void)
{
    CHECK(ae_drc_ppm(1000, 1000, 500) == 0);
    CHECK(ae_drc_ppm(0, 1000, 500) == -500);   /* empty: slow down */
    CHECK(ae_drc_ppm(2000, 1000, 500) == 500); /* twice target: speed up */
    CHECK(ae_drc_ppm(1500, 1000, 500) == 250);
    CHECK(ae_drc_ppm(500, 1000, 500) == -250);
    CHECK(ae_drc_ppm(100000, 1000, 500) == 500); /* clamped */
    CHECK(ae_drc_ppm(1234, 0, 500) == 0);        /* no target */
    CHECK(ae_drc_ppm(0xFFFFFFFFu, 1, 500) == 500);
}

int main(void)
{
    RUN(test_passthrough_equal_rates);
    RUN(test_empty_input);
    RUN(test_chunked_equals_one_shot);
    RUN(test_upsample_ratio);
    RUN(test_downsample_ratio);
    RUN(test_interpolation_exact);
    RUN(test_sine_frequency_preserved);
    RUN(test_ppm_changes_consumption);
    RUN(test_drc_ppm);
    return TEST_EXIT();
}

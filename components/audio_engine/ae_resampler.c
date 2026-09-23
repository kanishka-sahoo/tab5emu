#include "ae_resampler.h"

void ae_resampler_init(ae_resampler_t *r, unsigned in_rate, unsigned out_rate)
{
    r->base_step = ((uint64_t)in_rate << 32) / out_rate;
    r->step = r->base_step;
    r->pos = AE_ONE; /* first call loads s0 */
    r->s0[0] = r->s0[1] = 0;
}

void ae_resampler_set_ppm(ae_resampler_t *r, int32_t ppm)
{
    /* base_step < 2^34 for any sane ratio, so this can't overflow. */
    int64_t adj = (int64_t)r->base_step * ppm / 1000000;
    r->step = (uint64_t)((int64_t)r->base_step + adj);
}

size_t ae_resample(ae_resampler_t *r, const int16_t *in, size_t in_frames, size_t *in_used,
                   int16_t *out, size_t out_frames)
{
    size_t i = 0, n = 0;
    while (n < out_frames) {
        while (r->pos >= AE_ONE) {
            if (i >= in_frames) {
                goto done;
            }
            r->s0[0] = in[2 * i];
            r->s0[1] = in[2 * i + 1];
            i++;
            r->pos -= AE_ONE;
        }
        if (i >= in_frames) {
            break; /* need the next frame to interpolate towards */
        }
        /* 16-bit fraction is plenty for audio. */
        int32_t f = (int32_t)(r->pos >> 16);
        for (int c = 0; c < 2; c++) {
            int32_t a = r->s0[c];
            int32_t b = in[2 * i + (size_t)c];
            /* (b - a) * f needs 33 bits at full scale. */
            out[2 * n + (size_t)c] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * f) >> 16));
        }
        n++;
        r->pos += r->step;
    }
done:
    *in_used = i;
    return n;
}

int32_t ae_drc_ppm(uint32_t fill, uint32_t target, int32_t max_ppm)
{
    if (target == 0) {
        return 0;
    }
    int64_t ppm = ((int64_t)fill - (int64_t)target) * max_ppm / (int64_t)target;
    return (int32_t)(ppm > max_ppm ? max_ppm : ppm < -max_ppm ? -max_ppm : ppm);
}

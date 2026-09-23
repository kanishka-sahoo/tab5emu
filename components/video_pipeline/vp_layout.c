#include "vp_layout.h"

static unsigned umin(unsigned a, unsigned b)
{
    return a < b ? a : b;
}

/* Largest size <= want that the hardware scaler hits exactly: src * k /
 * steps for an integer k. */
static unsigned hw_fit(unsigned src, unsigned want, unsigned steps)
{
    unsigned k = (unsigned)((unsigned long)want * steps / src);
    while (k > 0 && ((unsigned long)src * k) % steps != 0) {
        k--;
    }
    return k ? (unsigned)((unsigned long)src * k / steps) : src;
}

static vp_layout_t centred(unsigned rw, unsigned rh, unsigned out_w, unsigned out_h, vp_scale_t s)
{
    vp_layout_t l;
    l.rect.w = (int)rw;
    l.rect.h = (int)rh;
    l.rect.x = (int)(out_w - rw) / 2;
    l.rect.y = (int)(out_h - rh) / 2;
    l.scale = s;
    return l;
}

/* Smooth modes: a target size, then the scaler that can reach it. */
static vp_layout_t smooth(unsigned w, unsigned h, unsigned rw, unsigned rh, unsigned out_w,
                          unsigned out_h, unsigned hw_steps)
{
    rw = umin(rw, out_w);
    rh = umin(rh, out_h);
    if (hw_steps) {
        return centred(hw_fit(w, rw, hw_steps), hw_fit(h, rh, hw_steps), out_w, out_h, VP_SCALE_HW);
    }
    return centred(rw, rh, out_w, out_h, VP_SCALE_CPU_NN);
}

vp_layout_t vp_layout_compute(vp_mode_t mode, unsigned w, unsigned h, unsigned out_w,
                              unsigned out_h, unsigned hw_steps)
{
    if (w == 0 || h == 0) {
        return centred(0, 0, out_w, out_h, VP_SCALE_CPU_NN);
    }
    const bool fits = w <= out_w && h <= out_h;

    if (mode == VP_MODE_PIXEL_PERFECT && fits) {
        /* Integer scale per axis, never wider than tall: 256x240 -> 3x3,
         * 512x224 (SNES hi-res) -> 2x3 (plan Phase 6). */
        unsigned sy = out_h / h;
        unsigned sx = umin(out_w / w, sy);
        vp_scale_t s = (sx == 3 && sy == 3) ? VP_SCALE_CPU_NN3 : VP_SCALE_CPU_NN;
        return centred(w * sx, h * sy, out_w, out_h, s);
    }
    if (mode == VP_MODE_ORIGINAL && fits) {
        return centred(w, h, out_w, out_h, VP_SCALE_CPU_NN);
    }
    if (mode == VP_MODE_4_3) {
        unsigned rh = out_h;
        unsigned rw = out_h * 4 / 3;
        if (rw > out_w) {
            rw = out_w;
            rh = out_w * 3 / 4;
        }
        return smooth(w, h, rw, rh, out_w, out_h, hw_steps);
    }
    if (mode == VP_MODE_STRETCH) {
        return smooth(w, h, out_w, out_h, out_w, out_h, hw_steps);
    }
    /* Fit, and the fallback for frames larger than the screen. */
    unsigned rw, rh;
    if ((unsigned long)out_w * h <= (unsigned long)out_h * w) {
        rw = out_w;
        rh = (unsigned)((unsigned long)h * out_w / w);
    } else {
        rh = out_h;
        rw = (unsigned)((unsigned long)w * out_h / h);
    }
    return smooth(w, h, rw, rh, out_w, out_h, hw_steps);
}

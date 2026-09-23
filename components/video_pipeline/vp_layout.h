/*
 * Display-mode geometry (spec §10): where the game image goes and which
 * scaler draws it. Pure functions, unit tested on the host.
 */
#pragma once

#include "retro_fb.h"
#include "video_pipeline.h"

typedef enum {
    VP_SCALE_CPU_NN3, /* retro_blit_rot_nn3_ex: exact 3x */
    VP_SCALE_CPU_NN,  /* retro_blit_rot_nn: any size */
    VP_SCALE_HW,      /* retro_video_hw_scale (PPA) */
} vp_scale_t;

typedef struct {
    retro_rect_t rect; /* landscape */
    vp_scale_t scale;
} vp_layout_t;

/*
 * w x h source into an out_w x out_h landscape screen. hw_steps is the
 * hardware scaler's step (16 = 1/16), or 0 for no hardware scaler; smooth
 * modes then use the CPU nearest-neighbour blit.
 */
vp_layout_t vp_layout_compute(vp_mode_t mode, unsigned w, unsigned h, unsigned out_w,
                              unsigned out_h, unsigned hw_steps);

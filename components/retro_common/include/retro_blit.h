/*
 * Software blits for RGB565 frames.
 *
 * The Tab5 panel scans out in portrait, so every landscape frame is rotated
 * 90 degrees on its way to the screen (plan D2). The rotation is fused into
 * the scale so it costs no extra pass (plan D4, "Pixel Perfect").
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "retro_fb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Flags for the _ex / general blits. */
enum {
    /* Darken the last output row of every source row to half brightness
     * (spec §10 "Scanline"). Only applies where each source row covers at
     * least two output rows. */
    RETRO_BLIT_SCANLINES = 1u << 0,
};

/*
 * Nearest-neighbour 3x upscale with a 90 degree rotation.
 *
 * src is w x h pixels with a row stride of src_stride pixels. dst receives a
 * block 3*h pixels wide and 3*w rows tall, row stride dst_stride pixels.
 * Source pixel (x, y) fills the 3x3 block at
 *   CW:  column 3*(h-1-y), row 3*x
 *   CCW: column 3*y,       row 3*(w-1-x)
 *
 * Writes each output row once, in order, then copies it twice. Rows are
 * written with 32-bit stores when dst rows are 4-byte aligned and h is even.
 */
void retro_blit_rot_nn3(uint16_t *dst, size_t dst_stride, const uint16_t *src, unsigned w,
                        unsigned h, size_t src_stride, retro_rot_t rot);

/* retro_blit_rot_nn3 with RETRO_BLIT_* flags. */
void retro_blit_rot_nn3_ex(uint16_t *dst, size_t dst_stride, const uint16_t *src, unsigned w,
                           unsigned h, size_t src_stride, retro_rot_t rot, unsigned flags);

/*
 * Nearest-neighbour scale to any landscape size out_w x out_h, with the
 * same rotation as retro_blit_rot_nn3. out_h is at most 1280 and h at most
 * 32767. dst points at the native top-left of the block, which is out_h
 * pixels wide and out_w rows tall. Landscape output pixel (X, Y) takes source (X*w/out_w, Y*h/out_h).
 * Output rows that repeat the previous one are copied.
 */
void retro_blit_rot_nn(uint16_t *dst, size_t dst_stride, unsigned out_w, unsigned out_h,
                       const uint16_t *src, unsigned w, unsigned h, size_t src_stride,
                       retro_rot_t rot, unsigned flags);

#ifdef __cplusplus
}
#endif

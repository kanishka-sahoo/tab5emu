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

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RETRO_ROT_CW,  /* landscape top edge -> portrait right edge */
    RETRO_ROT_CCW, /* landscape top edge -> portrait left edge */
} retro_rot_t;

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

#ifdef __cplusplus
}
#endif

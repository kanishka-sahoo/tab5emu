/*
 * Touch bring-up (plan Phase 1 item 6): multitouch count, coordinate update
 * rate while a finger moves, and the cost of one poll. Draws the touches so
 * the landscape mapping can be checked by eye.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hwtest_priv.h"
#include "retro_time.h"

#define MAX_PTS 5
#define DOT 12

static const uint16_t s_colors[MAX_PTS] = {C_RED, C_GREEN, C_BLUE, C_YELLOW, C_CYAN};

void hwtest_touch(int seconds)
{
    uint16_t *fb = retro_tab5_display_fb(1);
    if (!fb) {
        hw_result(HW_FAIL, "touch.test", "no display");
        return;
    }
    hw_display_acquire();
    memset(fb, 0, (size_t)RETRO_TAB5_PANEL_W * RETRO_TAB5_PANEL_H * 2);
    gfx_text(fb, 40, 40, 3, C_WHITE, C_WHITE, "TOUCH TEST");
    gfx_text(fb, 40, 90, 2, C_GREY, C_GREY, "Draw with up to 5 fingers. Keep one finger moving to");
    gfx_text(fb, 40, 116, 2, C_GREY, C_GREY, "measure the update rate. Dots should follow the fingers.");
    retro_tab5_display_show(fb);

    retro_tab5_touch_point_t pts[MAX_PTS], prev = {0};
    int max_pts = 0, errors = 0, polls = 0;
    uint32_t updates = 0;
    uint64_t moving_us = 0, poll_us = 0, last_update = 0;
    uint64_t end = retro_time_us() + (uint64_t)seconds * 1000000;

    while (retro_time_us() < end) {
        uint64_t t0 = retro_time_us();
        int n = retro_tab5_touch_read(pts, MAX_PTS);
        uint64_t t1 = retro_time_us();
        poll_us += t1 - t0;
        polls++;
        if (n < 0) {
            errors++;
        }
        if (n > max_pts) {
            max_pts = n;
        }
        if (n > 0) {
            /* A new coordinate for the first point = a new report. Only count
             * the time between reports that are close together (finger
             * moving), so pauses don't dilute the rate. */
            if (pts[0].x != prev.x || pts[0].y != prev.y) {
                if (last_update && t1 - last_update < 100000) {
                    moving_us += t1 - last_update;
                    updates++;
                }
                last_update = t1;
                prev = pts[0];
            }
            for (int i = 0; i < n; i++) {
                int x, y;
                gfx_from_native(pts[i].x, pts[i].y, &x, &y);
                gfx_fill(fb, x - DOT / 2, y - DOT / 2, DOT, DOT, s_colors[i % MAX_PTS]);
                gfx_flush(fb, x - DOT / 2, y - DOT / 2, DOT, DOT);
            }
        }
        vTaskDelay(1);
    }
    hw_display_release();

    float hz = moving_us ? (float)updates * 1e6f / (float)moving_us : 0;
    hw_result(max_pts >= 5 ? HW_PASS : HW_WARN, "touch.max_points", "%d simultaneous", max_pts);
    hw_result(updates ? HW_INFO : HW_WARN, "touch.report_hz", "%.0f Hz while moving (%lu updates)",
              (double)hz, (unsigned long)updates);
    hw_result(errors ? HW_WARN : HW_INFO, "touch.poll_us", "%.0f us per poll, %d errors in %d polls",
              polls ? (double)poll_us / polls : 0.0, errors, polls);
}

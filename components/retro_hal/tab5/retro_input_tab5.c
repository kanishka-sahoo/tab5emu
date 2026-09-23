/*
 * RetroHAL input on the Tab5: the touch screen in landscape coordinates.
 * There are no built-in keys (the Tab5 Keyboard is a later source).
 */
#include "sdkconfig.h"

#include "retro_fb.h"
#include "retro_input.h"
#include "retro_log.h"
#include "retro_tab5.h"

/* Poll interval while the interrupt line hasn't been seen to work. */
#define POLL_MS 10

static const retro_fb_t s_geom = {
    NULL, RETRO_TAB5_PANEL_W, RETRO_TAB5_PANEL_H, RETRO_TAB5_PANEL_W,
#if CONFIG_RETRO_TAB5_LANDSCAPE_CW
    RETRO_ROT_CW,
#else
    RETRO_ROT_CCW,
#endif
};

static uint32_t s_reports;

bool retro_touch_init(void)
{
    return retro_tab5_touch_init();
}

int retro_touch_read(retro_touch_point_t *pts, int max)
{
    retro_tab5_touch_point_t raw[RETRO_TOUCH_MAX_POINTS];
    if (max > RETRO_TOUCH_MAX_POINTS) {
        max = RETRO_TOUCH_MAX_POINTS;
    }
    int n = retro_tab5_touch_read(raw, max);
    for (int i = 0; i < n; i++) {
        int x, y;
        retro_fb_from_native(&s_geom, raw[i].x, raw[i].y, &x, &y);
        pts[i] = (retro_touch_point_t){(int16_t)x, (int16_t)y, raw[i].id};
    }
    if (n >= 0) {
        s_reports++;
    }
    return n;
}

bool retro_touch_wait(uint32_t timeout_ms)
{
    static bool s_irq_logged;
    if (!s_irq_logged && retro_tab5_touch_irqs() > 0) {
        s_irq_logged = true;
        RLOGI(INPUT, "touch interrupt seen; reads now follow it");
    }
    if (retro_tab5_touch_irqs() > 0) {
        return retro_tab5_touch_wait(timeout_ms);
    }
    /* No interrupt seen yet (GT911 board, or the line isn't working):
     * poll. The first interrupt switches this over. */
    retro_tab5_touch_wait(timeout_ms < POLL_MS ? timeout_ms : POLL_MS);
    return true;
}

uint32_t retro_touch_reports(void)
{
    return s_reports;
}

uint32_t retro_keys_read(void)
{
    return 0;
}

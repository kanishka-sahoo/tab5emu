/*
 * RetroHAL platform lifecycle. FROZEN (plan Phase 2).
 *
 * Tab5: board bring-up (I2C, IO expanders, rails). Host: SDL, the window and
 * the event loop, which must run on the main thread.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool retro_platform_init(void);

/* Process pending platform events (window, keyboard, mouse-as-touch). The
 * host calls it from the main thread about once per frame; on the Tab5 it
 * does nothing. Returns false once the user has asked to quit. */
bool retro_platform_pump(void);

void retro_platform_deinit(void);

#ifdef __cplusplus
}
#endif

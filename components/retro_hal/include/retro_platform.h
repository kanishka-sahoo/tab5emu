/*
 * RetroHAL platform lifecycle.
 *
 * PROVISIONAL: the video/audio/input/platform headers are a minimal Phase 0
 * surface used by the host build. They are redesigned and frozen in Phase 2;
 * only retro_log.h and retro_time.h are considered stable.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool retro_platform_init(void);

/* Process pending platform events (window, keyboard). Call once per frame.
 * Returns false once the user has asked to quit. */
bool retro_platform_pump(void);

void retro_platform_deinit(void);

#ifdef __cplusplus
}
#endif

/*
 * Shared state between the SDL2 host backends. SDL rendering and event
 * handling must stay on the main thread, so retro_platform_pump() drives
 * both.
 */
#pragma once

#include <SDL.h>

/* Render the frame buffer currently "on screen" (main thread only). */
void host_video_render(void);

/* Feed a mouse event to the emulated touch screen (main thread only). */
void host_touch_event(const SDL_Event *ev);

/* Keyboard state -> retro_keys_read() (main thread only). */
void host_keys_update(void);

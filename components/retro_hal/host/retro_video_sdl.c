/*
 * SDL2 video for the host build: a 1280x720 window standing in for the
 * rotated Tab5 panel, nearest-neighbour scaled with the aspect ratio kept.
 */
#include <SDL.h>

#include "retro_log.h"
#include "retro_video.h"

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static unsigned s_tex_w, s_tex_h;

bool retro_video_init(void)
{
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    s_window = SDL_CreateWindow("Tab5 Retro Console (host)", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, RETRO_VIDEO_OUT_WIDTH,
                                RETRO_VIDEO_OUT_HEIGHT, SDL_WINDOW_RESIZABLE);
    if (!s_window) {
        RLOGE(VIDEO, "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    s_renderer = SDL_CreateRenderer(s_window, -1, 0);
    if (!s_renderer) {
        RLOGE(VIDEO, "SDL_CreateRenderer failed: %s", SDL_GetError());
        retro_video_deinit();
        return false;
    }

    SDL_RendererInfo info;
    SDL_GetRendererInfo(s_renderer, &info);
    RLOGI(VIDEO, "window %dx%d, renderer %s", RETRO_VIDEO_OUT_WIDTH, RETRO_VIDEO_OUT_HEIGHT,
          info.name);
    return true;
}

void retro_video_present(const uint16_t *pixels, unsigned width, unsigned height, size_t pitch)
{
    if (!s_renderer) {
        return;
    }

    if (!s_texture || width != s_tex_w || height != s_tex_h) {
        if (s_texture) {
            SDL_DestroyTexture(s_texture);
        }
        s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565,
                                      SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
        if (!s_texture) {
            RLOGE(VIDEO, "SDL_CreateTexture failed: %s", SDL_GetError());
            return;
        }
        s_tex_w = width;
        s_tex_h = height;
    }
    SDL_UpdateTexture(s_texture, NULL, pixels, (int)pitch);

    int out_w, out_h;
    SDL_GetRendererOutputSize(s_renderer, &out_w, &out_h);
    float scale = SDL_min((float)out_w / (float)width, (float)out_h / (float)height);
    SDL_Rect dst;
    dst.w = (int)((float)width * scale);
    dst.h = (int)((float)height * scale);
    dst.x = (out_w - dst.w) / 2;
    dst.y = (out_h - dst.h) / 2;

    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, &dst);
    SDL_RenderPresent(s_renderer);
}

void retro_video_deinit(void)
{
    if (s_texture) {
        SDL_DestroyTexture(s_texture);
        s_texture = NULL;
    }
    if (s_renderer) {
        SDL_DestroyRenderer(s_renderer);
        s_renderer = NULL;
    }
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
    s_tex_w = s_tex_h = 0;
}

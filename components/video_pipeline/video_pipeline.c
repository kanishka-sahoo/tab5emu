/*
 * video_out task: mailbox -> scale + rotate -> display flip (see
 * video_pipeline.h).
 */
#include "video_pipeline.h"

#include <stdio.h>
#include <string.h>

#include "retro_blit.h"
#include "retro_gfx.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_time.h"
#include "retro_video.h"
#include "vp_layout.h"
#include "vp_mailbox.h"

#define OVERLAY_MAX 1024
#define MAX_DISPLAY_FBS 4
#define BORDER_COLOR 0x0000
/* Pixel Perfect with a hardware copier: native rows per SRAM strip (a
 * multiple of 3: 8 source columns). */
#define STRIP_ROWS 24
#define HW_WAIT_MS 50

static const char *const s_mode_names[VP_MODE_COUNT] = {
    "Pixel Perfect", "Original", "4:3", "Fit", "Stretch",
};

/* Indexed by vp_scale_t, then the strip variant of CPU_NN3. */
static const char *const s_path_names[] = {"cpu-3x", "cpu-nn", "ppa", "cpu-3x+dma"};
#define PATH_NN3_STRIPS 3

static struct {
    bool running;
    retro_task_t *task;
    retro_sem_t *new_frame;
    retro_mutex_t *lock; /* settings, overlay text, stats */
    vp_mailbox_t mb;
    uint16_t *native[VP_MAILBOX_BUFS];
    uint16_t *strip[2]; /* SRAM staging for the hardware-copy blit */
    unsigned max_w, max_h;
    retro_video_info_t info;

    /* Settings (under lock). */
    vp_mode_t mode;
    bool scanlines;
    char overlay[OVERLAY_MAX];
    uint32_t overlay_ver;

    /* Stats (under lock). */
    uint32_t rendered;
    uint64_t render_us_sum;
    uint32_t render_n, render_us_max;
    int last_path;
    unsigned src_w, src_h;
    retro_rect_t last_rect;
    bool hw_failed, copy_failed;
    vp_decor_fn decor;
    void *decor_ctx;
} s;

/* ---- Rendering (video_out task only) -------------------------------------------------- */

typedef struct {
    uint16_t *pixels;
    uint32_t gen; /* layout generation whose border this buffer has */
} fb_state_t;

static fb_state_t s_fbs[MAX_DISPLAY_FBS];
static uint32_t s_gen = 1;
static vp_layout_t s_layout;
static vp_mode_t s_cur_mode = VP_MODE_COUNT;
static bool s_cur_scan;
static unsigned s_cur_w, s_cur_h;

static char s_ov_text[OVERLAY_MAX];
static uint32_t s_ov_ver;
static retro_rect_t s_ov_box;

static fb_state_t *fb_state(uint16_t *pixels)
{
    for (int i = 0; i < MAX_DISPLAY_FBS; i++) {
        if (s_fbs[i].pixels == pixels) {
            return &s_fbs[i];
        }
    }
    for (int i = 0; i < MAX_DISPLAY_FBS; i++) {
        if (!s_fbs[i].pixels) {
            s_fbs[i].pixels = pixels;
            s_fbs[i].gen = 0;
            return &s_fbs[i];
        }
    }
    return &s_fbs[0];
}

/* Recompute the overlay box; a change in its size needs a border clear. */
static void overlay_update(void)
{
    uint32_t ver;
    retro_mutex_lock(s.lock);
    ver = s.overlay_ver;
    if (ver != s_ov_ver) {
        memcpy(s_ov_text, s.overlay, sizeof(s_ov_text));
    }
    retro_mutex_unlock(s.lock);
    if (ver == s_ov_ver) {
        return;
    }
    s_ov_ver = ver;

    int lines = 0, cols = 0, col = 0;
    for (const char *p = s_ov_text; *p; p++) {
        if (*p == '\n') {
            lines++;
            col = 0;
        } else if (++col > cols) {
            cols = col;
        }
    }
    if (s_ov_text[0] && (s_ov_text[strlen(s_ov_text) - 1] != '\n')) {
        lines++;
    }
    retro_rect_t box = {0, 0, 0, 0};
    if (lines > 0) {
        const retro_rect_t r = s_layout.rect;
        const int right = r.x + r.w;
        const int lw = (int)s.info.height; /* landscape width */
        if (lw - right >= 8 * 20 + 12) {
            box.x = right + 4;
            box.w = lw - right - 8;
        } else {
            box.x = 4;
            box.w = cols * 8 + 8;
        }
        box.y = 4;
        box.h = lines * 10 + 6;
    }
    if (box.x != s_ov_box.x || box.y != s_ov_box.y || box.w != s_ov_box.w || box.h != s_ov_box.h) {
        s_ov_box = box;
        s_gen++;
    }
}

static void overlay_draw(const retro_fb_t *fb)
{
    if (s_ov_box.w <= 0) {
        return;
    }
    retro_gfx_fill(fb, s_ov_box, 0x0000);
    int y = s_ov_box.y + 3;
    const char *p = s_ov_text;
    char line[160];
    while (*p) {
        size_t n = strcspn(p, "\n");
        size_t max = (size_t)(s_ov_box.w - 8) / 8;
        size_t k = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        k = k < max ? k : max;
        memcpy(line, p, k);
        line[k] = '\0';
        retro_gfx_text(fb, s_ov_box.x + 4, y, 1, 0xFFE0, 0xFFE0, line);
        y += 10;
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
}

/*
 * Pixel Perfect 3x through SRAM strips: the CPU builds STRIP_ROWS native rows
 * in on-chip RAM (fast; no PSRAM line fills), and a DMA engine writes them to
 * the frame buffer while the CPU builds the next strip. Bring-up found the
 * direct CPU blit bound by cached PSRAM writes. Strips span the whole panel
 * width, border columns included (black), so each one is a single contiguous
 * copy.
 */
static bool nn3_strips(const retro_fb_t *fb, const retro_rect_t *r, const uint16_t *src,
                       unsigned w, unsigned h, unsigned flags)
{
    const retro_rect_t n = retro_fb_rect_to_native(fb, r);
    const bool cw = fb->rot == RETRO_ROT_CW;
    const unsigned cols = STRIP_ROWS / 3;
    const unsigned fw = fb->width;
    unsigned k = 0;
    for (unsigned c0 = 0; c0 < w; c0 += cols, k++) {
        const unsigned nc = w - c0 < cols ? w - c0 : cols;
        uint16_t *buf = s.strip[k & 1];
        /* Copies finish in order: at most one outstanding means the one
         * from this buffer (two strips ago) is done. */
        if (k >= 2 && !retro_video_hw_sync(1, HW_WAIT_MS)) {
            return false;
        }
        if (n.w < (int)fw) {
            for (unsigned row = 0; row < 3 * nc; row++) {
                uint16_t *line = buf + (size_t)row * fw;
                memset(line, 0, (size_t)n.x * sizeof(uint16_t));
                memset(line + n.x + n.w, 0, (fw - (unsigned)(n.x + n.w)) * sizeof(uint16_t));
            }
        }
        /* Native rows run with landscape x on CW, against it on CCW. */
        const uint16_t *sub = src + (cw ? c0 : w - c0 - nc);
        retro_blit_rot_nn3_ex(buf + n.x, fw, sub, nc, h, s.max_w, fb->rot, flags);
        if (!retro_video_hw_copy(fb, 0, n.y + (int)(3 * c0), buf, fw, 3 * nc, fw)) {
            return false;
        }
    }
    return retro_video_hw_sync(0, HW_WAIT_MS);
}

static void render(const retro_fb_t *fb, const uint16_t *src, const vp_frame_meta_t *meta)
{
    vp_mode_t mode;
    bool scan;
    retro_mutex_lock(s.lock);
    mode = s.mode;
    scan = s.scanlines;
    retro_mutex_unlock(s.lock);

    const unsigned out_w = retro_fb_land_w(fb), out_h = retro_fb_land_h(fb);
    if (mode != s_cur_mode || scan != s_cur_scan || meta->width != s_cur_w ||
        meta->height != s_cur_h) {
        vp_layout_t l = vp_layout_compute(mode, meta->width, meta->height, out_w, out_h,
                                          s.info.hw_scale_steps);
        if (mode != s_cur_mode || scan != s_cur_scan) {
            RLOGI(VIDEO, "mode %s%s: %ux%u -> %dx%d at %d,%d (%s)", s_mode_names[mode],
                  scan ? " + scanlines" : "", meta->width, meta->height, l.rect.w, l.rect.h,
                  l.rect.x, l.rect.y, s_path_names[l.scale]);
        }
        s_layout = l;
        s_cur_mode = mode;
        s_cur_scan = scan;
        s_cur_w = meta->width;
        s_cur_h = meta->height;
        s_gen++;
        s_ov_ver = 0; /* re-place the overlay box */
    }
    overlay_update();

    const retro_rect_t r = s_layout.rect;
    int path = s_layout.scale;
    if (r.w > 0 && r.h > 0) {
        uint16_t *origin = retro_fb_rect_origin(fb, &r);
        const unsigned flags = scan ? RETRO_BLIT_SCANLINES : 0;
        if (path == VP_SCALE_CPU_NN3 && s.strip[0] && !s.copy_failed) {
            if (nn3_strips(fb, &r, src, meta->width, meta->height, flags)) {
                path = PATH_NN3_STRIPS;
            } else {
                RLOGW(VIDEO, "hardware copy failed; the CPU writes the frame buffer");
                retro_video_hw_sync(0, HW_WAIT_MS);
                s.copy_failed = true;
            }
        }
        if (path == VP_SCALE_HW) {
            if (s.hw_failed || !retro_video_hw_scale(fb, &r, src, meta->width, meta->height, s.max_w)) {
                if (!s.hw_failed) {
                    RLOGW(VIDEO, "hardware scaler failed; using the CPU");
                    s.hw_failed = true;
                }
                path = VP_SCALE_CPU_NN;
            }
        }
        if (path == VP_SCALE_CPU_NN3) {
            retro_blit_rot_nn3_ex(origin, fb->stride, src, meta->width, meta->height, s.max_w,
                                  fb->rot, flags);
        } else if (path == VP_SCALE_CPU_NN) {
            retro_blit_rot_nn(origin, fb->stride, (unsigned)r.w, (unsigned)r.h, src, meta->width,
                              meta->height, s.max_w, fb->rot, flags);
        }
    }

    /* After the scaler: the PPA invalidates the cache over the whole buffer
     * before it runs, which would drop earlier CPU writes. */
    fb_state_t *st = fb_state(fb->pixels);
    const bool cleared = st->gen != s_gen;
    if (cleared) {
        retro_gfx_fill_outside(fb, r, BORDER_COLOR);
        st->gen = s_gen;
    }
    vp_decor_fn decor = s.decor;
    if (decor) {
        decor(fb, &r, cleared, s.decor_ctx);
    }
    overlay_draw(fb);

    retro_mutex_lock(s.lock);
    s.last_path = path;
    s.src_w = meta->width;
    s.src_h = meta->height;
    s.last_rect = r;
    retro_mutex_unlock(s.lock);
}

static void video_task(void *arg)
{
    (void)arg;
    while (s.running) {
        if (!retro_sem_take(s.new_frame, 100)) {
            continue;
        }
        retro_fb_t fb;
        if (!retro_video_acquire(&fb, 100)) {
            RLOGW(VIDEO, "display acquire timed out");
            continue;
        }
        /* Take after acquire: waiting for the flip may have brought a newer
         * frame. */
        vp_frame_meta_t meta;
        const uint16_t *src = vp_mailbox_take(&s.mb, &meta);
        if (!src) {
            continue;
        }
        uint64_t t0 = retro_time_us();
        render(&fb, src, &meta);
        retro_video_present(&fb);
        uint32_t us = (uint32_t)(retro_time_us() - t0);

        retro_mutex_lock(s.lock);
        s.rendered++;
        s.render_us_sum += us;
        s.render_n++;
        if (us > s.render_us_max) {
            s.render_us_max = us;
        }
        retro_mutex_unlock(s.lock);
    }
}

/* ---- Public API ------------------------------------------------------------------------ */

bool vp_start(const vp_config_t *cfg)
{
    if (s.running) {
        return true;
    }
    retro_video_get_info(&s.info);
    s.max_w = cfg->max_width;
    s.max_h = cfg->max_height;
    const size_t bytes = (size_t)s.max_w * s.max_h * sizeof(uint16_t);
    if (s.info.hw_copy) {
        /* Before the native buffers: these must be in SRAM to help. */
        for (int i = 0; i < 2; i++) {
            s.strip[i] = retro_mem_alloc((size_t)STRIP_ROWS * s.info.width * sizeof(uint16_t),
                                         RETRO_MEM_INTERNAL | RETRO_MEM_DMA);
        }
        if (!s.strip[0] || !s.strip[1]) {
            RLOGW(VIDEO, "no SRAM for blit strips; the CPU writes the frame buffer");
            retro_mem_free(s.strip[0]);
            retro_mem_free(s.strip[1]);
            s.strip[0] = s.strip[1] = NULL;
        }
    }
    for (int i = 0; i < VP_MAILBOX_BUFS; i++) {
        /* Native frames in SRAM when they fit: a PSRAM source costs the
         * CPU blit ~2.4 ms more per frame (bring-up). */
        s.native[i] = retro_mem_alloc(bytes, RETRO_MEM_INTERNAL | RETRO_MEM_FALLBACK);
        if (!s.native[i]) {
            RLOGE(VIDEO, "no memory for native frame buffers");
            vp_stop();
            return false;
        }
        if (!retro_mem_is_internal(s.native[i])) {
            RLOGW(VIDEO, "native frame buffer %d in PSRAM (SRAM full)", i);
        }
    }
    vp_mailbox_init(&s.mb, s.native[0], s.native[1], s.native[2]);
    s.lock = retro_mutex_create();
    s.new_frame = retro_sem_create(1, 0);
    if (!s.lock || !s.new_frame) {
        vp_stop();
        return false;
    }
    memset(s_fbs, 0, sizeof(s_fbs));
    s_cur_mode = VP_MODE_COUNT;
    s.running = true;
    const retro_task_config_t tc = {
        .name = "video_out",
        .fn = video_task,
        .stack_bytes = 8192,
        .priority = cfg->priority,
        .core = cfg->core,
    };
    s.task = retro_task_create(&tc);
    if (!s.task) {
        s.running = false;
        vp_stop();
        return false;
    }
    RLOGI(VIDEO, "pipeline up: native %ux%u x%d, display %ux%u %.2f Hz, hw scaler %s", s.max_w,
          s.max_h, VP_MAILBOX_BUFS, s.info.width, s.info.height, (double)s.info.refresh_hz,
          s.info.hw_scale_steps ? "yes" : "no");
    return true;
}

void vp_stop(void)
{
    if (s.running) {
        s.running = false;
        retro_task_join(s.task);
        s.task = NULL;
    }
    retro_sem_destroy(s.new_frame);
    retro_mutex_destroy(s.lock);
    s.new_frame = NULL;
    s.lock = NULL;
    for (int i = 0; i < VP_MAILBOX_BUFS; i++) {
        retro_mem_free(s.native[i]);
        s.native[i] = NULL;
    }
    for (int i = 0; i < 2; i++) {
        retro_mem_free(s.strip[i]);
        s.strip[i] = NULL;
    }
}

void vp_set_mode(vp_mode_t mode)
{
    if (mode < VP_MODE_COUNT && s.lock) {
        retro_mutex_lock(s.lock);
        s.mode = mode;
        retro_mutex_unlock(s.lock);
    }
}

vp_mode_t vp_get_mode(void)
{
    return s.mode;
}

const char *vp_mode_name(vp_mode_t mode)
{
    return mode < VP_MODE_COUNT ? s_mode_names[mode] : "?";
}

void vp_set_scanlines(bool on)
{
    if (s.lock) {
        retro_mutex_lock(s.lock);
        s.scanlines = on;
        retro_mutex_unlock(s.lock);
    }
}

bool vp_get_scanlines(void)
{
    return s.scanlines;
}

void vp_frame_begin(retro_video_frame_t *out)
{
    out->pixels = vp_mailbox_write_buf(&s.mb);
    out->stride = s.max_w;
    out->width = s.max_w;
    out->height = s.max_h;
}

void vp_frame_publish(unsigned width, unsigned height)
{
    width = width > s.max_w ? s.max_w : width;
    height = height > s.max_h ? s.max_h : height;
    vp_mailbox_publish(&s.mb, width, height);
    retro_sem_give(s.new_frame);
}

void vp_set_decor(vp_decor_fn fn, void *ctx)
{
    s.decor_ctx = ctx;
    s.decor = fn;
}

void vp_overlay_set_text(const char *text)
{
    if (!s.lock) {
        return;
    }
    retro_mutex_lock(s.lock);
    snprintf(s.overlay, sizeof(s.overlay), "%s", text ? text : "");
    s.overlay_ver++;
    retro_mutex_unlock(s.lock);
}

void vp_get_stats(vp_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s.lock) {
        return;
    }
    out->published = atomic_load(&s.mb.published);
    out->dropped = atomic_load(&s.mb.dropped);
    out->vsyncs = retro_video_frame_count();
    out->refresh_hz = s.info.refresh_hz;
    retro_mutex_lock(s.lock);
    out->rendered = s.rendered;
    out->render_ms_avg = s.render_n ? (float)s.render_us_sum / (float)s.render_n / 1000.0f : 0.0f;
    out->render_ms_max = (float)s.render_us_max / 1000.0f;
    out->path = s_path_names[s.last_path];
    out->src_w = s.src_w;
    out->src_h = s.src_h;
    out->dst_w = s.last_rect.w;
    out->dst_h = s.last_rect.h;
    s.render_us_sum = 0;
    s.render_n = 0;
    s.render_us_max = 0;
    retro_mutex_unlock(s.lock);
}

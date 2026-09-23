/*
 * Controller mapping database: defaults by position and per-VID:PID remaps
 * stored in /retro/config/controllers.json (spec §18).
 *
 *   {
 *     "version": 1,
 *     "controllers": [
 *       { "id": "045e:0b12", "name": "Xbox Series", "stick_dpad": true,
 *         "map": { "south": "b", "east": "a", ..., "guide": "menu" } }
 *     ]
 *   }
 *
 * Controls missing from "map" keep their default. Targets: up down left
 * right a b x y l r start select menu none.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm_priv.h"
#include "retro_json.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_storage.h"

#define DB_MAX 16
#define JSON_MAX_TOKENS 512
#define JSON_MAX_BYTES 16384

typedef struct {
    bool used;
    uint16_t vid, pid;
    char name[32];
    cm_mapping_t map;
} entry_t;

static entry_t s_db[DB_MAX];

static const char *const s_phys_names[CM_BTN_COUNT] = {
    "up", "down", "left", "right", "south", "east", "west", "north", "l1",
    "r1", "l2", "r2", "l3", "r3", "start", "select", "guide",
};

static const struct {
    const char *name;
    uint16_t button;
    uint8_t hotkey;
} s_targets[] = {
    {"none", 0, 0},
    {"up", RETRO_BTN_UP, 0},
    {"down", RETRO_BTN_DOWN, 0},
    {"left", RETRO_BTN_LEFT, 0},
    {"right", RETRO_BTN_RIGHT, 0},
    {"a", RETRO_BTN_A, 0},
    {"b", RETRO_BTN_B, 0},
    {"x", RETRO_BTN_X, 0},
    {"y", RETRO_BTN_Y, 0},
    {"l", RETRO_BTN_L, 0},
    {"r", RETRO_BTN_R, 0},
    {"start", RETRO_BTN_START, 0},
    {"select", RETRO_BTN_SELECT, 0},
    {"menu", 0, RETRO_HOTKEY_MENU},
};

#define N_TARGETS (int)(sizeof(s_targets) / sizeof(s_targets[0]))

void cm_mapping_default(cm_mapping_t *m)
{
    memset(m, 0, sizeof(*m));
    m->buttons[CM_BTN_UP] = RETRO_BTN_UP;
    m->buttons[CM_BTN_DOWN] = RETRO_BTN_DOWN;
    m->buttons[CM_BTN_LEFT] = RETRO_BTN_LEFT;
    m->buttons[CM_BTN_RIGHT] = RETRO_BTN_RIGHT;
    /* By position (plan Phase 2): Xbox B -> A, A -> B, Y -> X, X -> Y. */
    m->buttons[CM_BTN_EAST] = RETRO_BTN_A;
    m->buttons[CM_BTN_SOUTH] = RETRO_BTN_B;
    m->buttons[CM_BTN_NORTH] = RETRO_BTN_X;
    m->buttons[CM_BTN_WEST] = RETRO_BTN_Y;
    m->buttons[CM_BTN_L1] = RETRO_BTN_L;
    m->buttons[CM_BTN_R1] = RETRO_BTN_R;
    m->buttons[CM_BTN_START] = RETRO_BTN_START;
    m->buttons[CM_BTN_SELECT] = RETRO_BTN_SELECT;
    m->hotkeys[CM_BTN_GUIDE] = RETRO_HOTKEY_MENU;
    m->stick_dpad = true;
}

void cm_mapping_db_init(void)
{
    memset(s_db, 0, sizeof(s_db));
}

static entry_t *find(uint16_t vid, uint16_t pid)
{
    for (int i = 0; i < DB_MAX; i++) {
        if (s_db[i].used && s_db[i].vid == vid && s_db[i].pid == pid) {
            return &s_db[i];
        }
    }
    return NULL;
}

void cm_mapping_get_locked(uint16_t vid, uint16_t pid, cm_mapping_t *out)
{
    entry_t *e = find(vid, pid);
    if (e) {
        *out = e->map;
    } else {
        cm_mapping_default(out);
    }
}

void cm_mapping_get(uint16_t vid, uint16_t pid, cm_mapping_t *out)
{
    cm_lock();
    cm_mapping_get_locked(vid, pid, out);
    cm_unlock();
}

static bool set_locked(uint16_t vid, uint16_t pid, const char *name, const cm_mapping_t *m)
{
    entry_t *e = find(vid, pid);
    for (int i = 0; i < DB_MAX && !e; i++) {
        if (!s_db[i].used) {
            e = &s_db[i];
        }
    }
    if (!e) {
        return false;
    }
    e->used = true;
    e->vid = vid;
    e->pid = pid;
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "");
    e->map = *m;
    cm_remap_connected_locked(vid, pid, m);
    return true;
}

bool cm_mapping_set(uint16_t vid, uint16_t pid, const char *name, const cm_mapping_t *m)
{
    cm_lock();
    bool ok = set_locked(vid, pid, name, m);
    cm_unlock();
    return ok;
}

static int target_index(uint16_t button, uint8_t hotkey)
{
    for (int t = 0; t < N_TARGETS; t++) {
        if (s_targets[t].button == button && s_targets[t].hotkey == hotkey) {
            return t;
        }
    }
    return 0;
}

bool cm_mapping_parse(const char *js, size_t len)
{
    retro_json_tok_t *toks = retro_mem_alloc(JSON_MAX_TOKENS * sizeof(*toks), RETRO_MEM_ANY);
    if (!toks) {
        return false;
    }
    int n = retro_json_parse(js, len, toks, JSON_MAX_TOKENS);
    int list = n > 0 ? retro_json_get(js, toks, n, 0, "controllers") : -1;
    if (list < 0 || toks[list].type != RETRO_JSON_ARRAY) {
        RLOGW(INPUT, "controllers.json: not a controller list (%d)", n);
        retro_mem_free(toks);
        return false;
    }
    int loaded = 0;
    cm_lock();
    for (int i = 0; i < toks[list].size; i++) {
        int c = retro_json_at(toks, n, list, i);
        char id[16] = "", name[32] = "";
        int t_id = retro_json_get(js, toks, n, c, "id");
        unsigned vid, pid;
        if (t_id < 0 || !retro_json_str(js, &toks[t_id], id, sizeof(id)) ||
            sscanf(id, "%4x:%4x", &vid, &pid) != 2) {
            continue;
        }
        int t_name = retro_json_get(js, toks, n, c, "name");
        if (t_name >= 0) {
            retro_json_str(js, &toks[t_name], name, sizeof(name));
        }
        cm_mapping_t m;
        cm_mapping_default(&m);
        int t_sd = retro_json_get(js, toks, n, c, "stick_dpad");
        if (t_sd >= 0) {
            retro_json_bool(js, &toks[t_sd], &m.stick_dpad);
        }
        int map = retro_json_get(js, toks, n, c, "map");
        for (int b = 0; b < CM_BTN_COUNT && map >= 0; b++) {
            int v = retro_json_get(js, toks, n, map, s_phys_names[b]);
            char tgt[16];
            if (v < 0 || !retro_json_str(js, &toks[v], tgt, sizeof(tgt))) {
                continue;
            }
            for (int t = 0; t < N_TARGETS; t++) {
                if (strcmp(tgt, s_targets[t].name) == 0) {
                    m.buttons[b] = s_targets[t].button;
                    m.hotkeys[b] = s_targets[t].hotkey;
                }
            }
        }
        loaded += set_locked((uint16_t)vid, (uint16_t)pid, name, &m);
    }
    cm_unlock();
    retro_mem_free(toks);
    RLOGI(INPUT, "controller mappings: %d loaded", loaded);
    return true;
}

size_t cm_mapping_serialize(char *buf, size_t cap)
{
    retro_json_writer_t w;
    retro_json_w_init(&w, buf, cap);
    retro_json_w_object(&w, NULL);
    retro_json_w_int(&w, "version", 1);
    retro_json_w_array(&w, "controllers");
    cm_lock();
    for (int i = 0; i < DB_MAX; i++) {
        const entry_t *e = &s_db[i];
        if (!e->used) {
            continue;
        }
        char id[16];
        snprintf(id, sizeof(id), "%04x:%04x", e->vid, e->pid);
        retro_json_w_object(&w, NULL);
        retro_json_w_str(&w, "id", id);
        retro_json_w_str(&w, "name", e->name);
        retro_json_w_bool(&w, "stick_dpad", e->map.stick_dpad);
        retro_json_w_object(&w, "map");
        for (int b = 0; b < CM_BTN_COUNT; b++) {
            int t = target_index(e->map.buttons[b], e->map.hotkeys[b]);
            retro_json_w_str(&w, s_phys_names[b], s_targets[t].name);
        }
        retro_json_w_end(&w);
        retro_json_w_end(&w);
    }
    cm_unlock();
    retro_json_w_end(&w);
    retro_json_w_end(&w);
    return retro_json_w_finish(&w);
}

bool cm_mapping_load(const char *path)
{
    size_t len;
    char *js = retro_storage_read_file(path, &len, RETRO_MEM_ANY);
    if (!js) {
        return false;
    }
    bool ok = cm_mapping_parse(js, len);
    retro_mem_free(js);
    return ok;
}

bool cm_mapping_save(const char *path)
{
    char *buf = retro_mem_alloc(JSON_MAX_BYTES, RETRO_MEM_ANY);
    if (!buf) {
        return false;
    }
    size_t len = cm_mapping_serialize(buf, JSON_MAX_BYTES);
    bool ok = len > 0 && retro_storage_write_atomic(path, buf, len);
    retro_mem_free(buf);
    return ok;
}

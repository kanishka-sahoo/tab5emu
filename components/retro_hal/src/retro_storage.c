/*
 * Shared RetroHAL storage: logical volume paths and crash-safe files (see
 * retro_storage.h). Plain C library I/O, which the ESP-IDF VFS and POSIX
 * both provide.
 */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "retro_log.h"
#include "retro_os.h"
#include "retro_storage.h"
#include "retro_storage_port.h"

#define ROOT_LEN (sizeof(RETRO_STORAGE_ROOT) - 1)

bool retro_storage_init(void)
{
    int n;
    retro_storage_port_volumes(&n);
    bool any = false;
    for (int i = 0; i < n; i++) {
        any |= retro_storage_port_mount(i);
    }
    return any;
}

int retro_storage_volume_count(void)
{
    int n;
    retro_storage_port_volumes(&n);
    return n;
}

bool retro_storage_volume_info(int index, retro_volume_info_t *out)
{
    int n;
    const retro_storage_port_vol_t *v = retro_storage_port_volumes(&n);
    if (index < 0 || index >= n) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->name = v[index].name;
    out->mount = v[index].mount;
    out->removable = v[index].removable;
    out->mounted = retro_storage_port_mounted(index);
    if (out->mounted) {
        retro_storage_port_space(index, out);
    }
    return true;
}

/* Volume index for a logical path and the offset of the rest of the path
 * after "/storage/<name>", or -1. */
static int find_volume(const char *path, size_t *rest)
{
    if (!path || strncmp(path, RETRO_STORAGE_ROOT "/", ROOT_LEN + 1) != 0) {
        return -1;
    }
    int n;
    const retro_storage_port_vol_t *v = retro_storage_port_volumes(&n);
    const char *name = path + ROOT_LEN + 1;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(v[i].name);
        if (strncmp(name, v[i].name, len) == 0 && (name[len] == '/' || name[len] == '\0')) {
            *rest = (size_t)(name + len - path);
            return i;
        }
    }
    return -1;
}

bool retro_storage_mounted(const char *volume_name)
{
    int n;
    const retro_storage_port_vol_t *v = retro_storage_port_volumes(&n);
    for (int i = 0; i < n; i++) {
        if (strcmp(v[i].name, volume_name) == 0) {
            return retro_storage_port_mounted(i);
        }
    }
    return false;
}

bool retro_storage_resolve(const char *path, char *out, size_t cap)
{
    size_t rest;
    int i = find_volume(path, &rest);
    if (i < 0 || !retro_storage_port_mounted(i)) {
        return false;
    }
    int n;
    const retro_storage_port_vol_t *v = retro_storage_port_volumes(&n);
    int len = snprintf(out, cap, "%s%s", v[i].native_root, path + rest);
    return len > 0 && (size_t)len < cap;
}

FILE *retro_storage_fopen(const char *path, const char *mode)
{
    char native[RETRO_PATH_MAX];
    if (!retro_storage_resolve(path, native, sizeof(native))) {
        return NULL;
    }
    return fopen(native, mode);
}

bool retro_storage_exists(const char *path)
{
    return retro_storage_size(path) >= 0;
}

long retro_storage_size(const char *path)
{
    char native[RETRO_PATH_MAX];
    struct stat st;
    if (!retro_storage_resolve(path, native, sizeof(native)) || stat(native, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

bool retro_storage_remove(const char *path)
{
    char native[RETRO_PATH_MAX];
    return retro_storage_resolve(path, native, sizeof(native)) && unlink(native) == 0;
}

bool retro_storage_mkdirs(const char *path)
{
    size_t rest;
    char native[RETRO_PATH_MAX];
    if (find_volume(path, &rest) < 0 || !retro_storage_resolve(path, native, sizeof(native))) {
        return false;
    }
    /* Create each component below the volume root. */
    char *p = native + strlen(native) - strlen(path + rest);
    if (*p == '/') {
        p++;
    }
    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(native, 0775) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    struct stat st;
    return (mkdir(native, 0775) == 0 || errno == EEXIST) && stat(native, &st) == 0 &&
           S_ISDIR(st.st_mode);
}

/* path + suffix into buf. */
static bool with_suffix(char *buf, size_t cap, const char *path, const char *suffix)
{
    int n = snprintf(buf, cap, "%s%s", path, suffix);
    return n > 0 && (size_t)n < cap;
}

static bool mkdirs_parent(const char *path)
{
    char dir[RETRO_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return false;
    }
    *slash = '\0';
    return retro_storage_mkdirs(dir);
}

bool retro_storage_recover(const char *path)
{
    char native[RETRO_PATH_MAX], tmp[RETRO_PATH_MAX], fresh[RETRO_PATH_MAX];
    if (!retro_storage_resolve(path, native, sizeof(native)) ||
        !with_suffix(tmp, sizeof(tmp), native, ".tmp") ||
        !with_suffix(fresh, sizeof(fresh), native, ".new")) {
        return false;
    }
    struct stat st;
    if (stat(fresh, &st) == 0) {
        /* Step 3 was interrupted: .new is complete and newer than path. */
        unlink(native);
        if (rename(fresh, native) != 0) {
            RLOGE(SD, "recover %s: rename failed (%d)", path, errno);
        } else {
            RLOGW(SD, "recovered %s from an interrupted write", path);
        }
    }
    if (stat(tmp, &st) == 0) {
        unlink(tmp); /* partial by definition */
    }
    return stat(native, &st) == 0;
}

bool retro_storage_write_atomic(const char *path, const void *data, size_t len)
{
    char native[RETRO_PATH_MAX], tmp[RETRO_PATH_MAX], fresh[RETRO_PATH_MAX];
    if (!retro_storage_resolve(path, native, sizeof(native)) ||
        !with_suffix(tmp, sizeof(tmp), native, ".tmp") ||
        !with_suffix(fresh, sizeof(fresh), native, ".new")) {
        return false;
    }
    mkdirs_parent(path);
    retro_storage_recover(path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        RLOGE(SD, "can't create %s (%d)", tmp, errno);
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = ok && fflush(f) == 0;
    ok = ok && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        RLOGE(SD, "write %s failed (%d)", tmp, errno);
        unlink(tmp);
        return false;
    }
    if (rename(tmp, fresh) != 0) {
        RLOGE(SD, "rename %s failed (%d)", tmp, errno);
        unlink(tmp);
        return false;
    }
    unlink(native);
    if (rename(fresh, native) != 0) {
        /* .new is complete; the next recover() finishes the job. */
        RLOGE(SD, "rename %s failed (%d)", fresh, errno);
        return false;
    }
    return true;
}

void *retro_storage_read_file(const char *path, size_t *len, unsigned caps)
{
    if (!retro_storage_recover(path)) {
        return NULL;
    }
    long size = retro_storage_size(path);
    FILE *f = size >= 0 ? retro_storage_fopen(path, "rb") : NULL;
    if (!f) {
        return NULL;
    }
    char *buf = retro_mem_alloc((size_t)size + 1, caps);
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        retro_mem_free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[size] = '\0';
        if (len) {
            *len = (size_t)size;
        }
    }
    return buf;
}

static bool has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), s = strlen(suffix);
    return n > s && strcmp(name + n - s, suffix) == 0;
}

#define RECOVER_BATCH 16
#define RECOVER_MAX_PASSES 64 /* gives up on files it can't repair */

int retro_storage_recover_dir(const char *dir, bool recursive)
{
    char native[RETRO_PATH_MAX];
    if (!retro_storage_resolve(dir, native, sizeof(native))) {
        return 0;
    }
    int fixed = 0;
    bool more = true;
    int passes = 0;
    /* Renaming entries while readdir() walks the directory isn't safe on
     * FAT, so collect a batch of names, close, repair, and rescan if the
     * batch was full. Subdirectories are visited on every pass: a pass that
     * fills its batch stops early and may not have reached them. */
    while (more) {
        DIR *d = opendir(native);
        if (!d) {
            break;
        }
        char (*batch)[RETRO_PATH_MAX] = malloc(RECOVER_BATCH * RETRO_PATH_MAX);
        if (!batch) {
            closedir(d);
            break;
        }
        int n = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < RECOVER_BATCH) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            char child[RETRO_PATH_MAX];
            if (snprintf(child, sizeof(child), "%s/%s", dir, e->d_name) >= (int)sizeof(child)) {
                continue;
            }
            bool is_dir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN) {
                char cn[RETRO_PATH_MAX];
                struct stat st;
                is_dir = retro_storage_resolve(child, cn, sizeof(cn)) && stat(cn, &st) == 0 &&
                         S_ISDIR(st.st_mode);
            }
            if (is_dir) {
                if (recursive) {
                    fixed += retro_storage_recover_dir(child, true);
                }
            } else if (has_suffix(child, ".new") || has_suffix(child, ".tmp")) {
                memcpy(batch[n++], child, sizeof(child));
            }
        }
        more = n == RECOVER_BATCH && ++passes < RECOVER_MAX_PASSES;
        closedir(d);
        for (int i = 0; i < n; i++) {
            bool had_new = has_suffix(batch[i], ".new");
            batch[i][strlen(batch[i]) - 4] = '\0';
            retro_storage_recover(batch[i]);
            fixed += had_new;
        }
        free(batch);
    }
    return fixed;
}

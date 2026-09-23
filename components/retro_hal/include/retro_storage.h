/*
 * RetroHAL storage: logical volumes and crash-safe files. FROZEN (plan
 * Phase 2).
 *
 * Paths are logical and start at RETRO_STORAGE_ROOT, one directory per
 * volume (spec §20): "/storage/sd/retro/roms/nes/...". v1 has only the
 * microSD card; USB drives become /storage/usb0 and so on later. On the Tab5
 * the logical path is the real VFS path; the host maps each volume to a
 * directory, so always open files through these wrappers.
 *
 * Crash-safe writes (plan D6, adjusted by the Phase 1 finding that FAT's
 * rename() won't replace an existing file):
 *   1. write <path>.tmp, fsync
 *   2. rename <path>.tmp -> <path>.new   (now known complete)
 *   3. unlink <path>, rename <path>.new -> <path>
 * A power cut at any point leaves either the old file, or a complete .new
 * that retro_storage_recover() promotes. A leftover .tmp is always partial
 * and is deleted.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETRO_STORAGE_ROOT "/storage"
#define RETRO_STORAGE_SD RETRO_STORAGE_ROOT "/sd"

/* Longest logical or native path, including the NUL. */
#define RETRO_PATH_MAX 256

typedef struct {
    const char *name;  /* "sd" */
    const char *mount; /* "/storage/sd" */
    bool mounted;
    bool removable;
    uint64_t total_bytes, free_bytes; /* 0 if unknown */
    unsigned cluster_bytes;           /* 0 if unknown */
} retro_volume_info_t;

/* Mount every known volume. Returns true if at least one is mounted.
 * Safe to call again (e.g. after a card is inserted). */
bool retro_storage_init(void);

int retro_storage_volume_count(void);
bool retro_storage_volume_info(int index, retro_volume_info_t *out);
bool retro_storage_mounted(const char *volume_name);

/* Logical path -> path for the C library. False if the path isn't under a
 * mounted volume or doesn't fit. */
bool retro_storage_resolve(const char *path, char *out, size_t cap);

FILE *retro_storage_fopen(const char *path, const char *mode);
bool retro_storage_exists(const char *path);
/* File size in bytes, -1 if missing. */
long retro_storage_size(const char *path);
bool retro_storage_remove(const char *path);
/* Create path and any missing parents. */
bool retro_storage_mkdirs(const char *path);

/* Crash-safe replace of path's contents (see above). Creates parent
 * directories. */
bool retro_storage_write_atomic(const char *path, const void *data, size_t len);

/*
 * Read a whole file into a new buffer (retro_mem_alloc with caps, plus a
 * NUL terminator not counted in *len). Runs retro_storage_recover() first.
 * Free with retro_mem_free(). NULL if missing or unreadable.
 */
void *retro_storage_read_file(const char *path, size_t *len, unsigned caps);

/* Finish an interrupted atomic write of path, if any. Returns true if
 * path exists afterwards. */
bool retro_storage_recover(const char *path);

/* retro_storage_recover() for every file in dir (optionally recursive).
 * Returns the number of files repaired. */
int retro_storage_recover_dir(const char *dir, bool recursive);

#ifdef __cplusplus
}
#endif

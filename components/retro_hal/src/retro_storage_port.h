/*
 * Hooks each backend (tab5/, host/) provides to the shared storage code.
 */
#pragma once

#include <stdbool.h>

#include "retro_storage.h"

typedef struct {
    const char *name;        /* "sd" */
    const char *mount;       /* logical root, RETRO_STORAGE_ROOT "/sd" */
    const char *native_root; /* where the C library finds it */
    bool removable;
} retro_storage_port_vol_t;

/* The fixed volume table. */
const retro_storage_port_vol_t *retro_storage_port_volumes(int *count);

/* Mount volume i if it isn't already. Returns true if mounted. */
bool retro_storage_port_mount(int i);
bool retro_storage_port_mounted(int i);

/* Fill total/free/cluster size where known. */
void retro_storage_port_space(int i, retro_volume_info_t *out);

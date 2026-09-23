/*
 * Host storage: the "sd" volume is a directory, $RETRO_SD_ROOT or ./sdcard.
 */
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "retro_log.h"
#include "retro_storage_port.h"

static retro_storage_port_vol_t s_vols[1] = {
    {"sd", RETRO_STORAGE_SD, "sdcard", true},
};
static bool s_mounted[1];

const retro_storage_port_vol_t *retro_storage_port_volumes(int *count)
{
    const char *root = getenv("RETRO_SD_ROOT");
    if (root && *root) {
        s_vols[0].native_root = root;
    }
    *count = 1;
    return s_vols;
}

bool retro_storage_port_mount(int i)
{
    int n;
    retro_storage_port_volumes(&n);
    if (i < 0 || i >= n) {
        return false;
    }
    if (!s_mounted[i]) {
        if (mkdir(s_vols[i].native_root, 0775) != 0 && errno != EEXIST) {
            RLOGE(SD, "can't create %s", s_vols[i].native_root);
            return false;
        }
        s_mounted[i] = true;
        RLOGI(SD, "%s -> %s", s_vols[i].mount, s_vols[i].native_root);
    }
    return true;
}

bool retro_storage_port_mounted(int i)
{
    return i >= 0 && i < 1 && s_mounted[i];
}

void retro_storage_port_space(int i, retro_volume_info_t *out)
{
    (void)i;
    (void)out;
}

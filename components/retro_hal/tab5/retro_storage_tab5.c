/*
 * Tab5 storage: the microSD card, mounted by the BSP straight at its logical
 * path (CONFIG_BSP_SD_MOUNT_POINT = RETRO_STORAGE_SD), so no translation.
 */
#include <string.h>

#include "retro_log.h"
#include "retro_storage_port.h"
#include "retro_tab5.h"

/* retro_tab5_sd_mount() checks CONFIG_BSP_SD_MOUNT_POINT against this. */
_Static_assert(sizeof(RETRO_TAB5_SD_MOUNT) == sizeof(RETRO_STORAGE_SD),
               "the SD card must mount at its logical path");

static const retro_storage_port_vol_t s_vols[] = {
    {"sd", RETRO_STORAGE_SD, RETRO_TAB5_SD_MOUNT, true},
};

const retro_storage_port_vol_t *retro_storage_port_volumes(int *count)
{
    *count = 1;
    return s_vols;
}

bool retro_storage_port_mount(int i)
{
    return i == 0 && retro_tab5_sd_mount();
}

bool retro_storage_port_mounted(int i)
{
    return i == 0 && retro_tab5_sd_mounted();
}

void retro_storage_port_space(int i, retro_volume_info_t *out)
{
    retro_tab5_sd_info_t info;
    if (i == 0 && retro_tab5_sd_info(&info)) {
        out->total_bytes = info.capacity_bytes;
        out->cluster_bytes = info.cluster_bytes;
    }
}

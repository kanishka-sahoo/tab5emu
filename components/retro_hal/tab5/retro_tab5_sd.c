/*
 * Tab5 microSD: SDMMC slot 0, 4-bit, through the BSP (which also powers the
 * card from on-chip LDO 4).
 */
#include <string.h>

#include <stdio.h>

#include "bsp/m5stack_tab5.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#include "retro_log.h"
#include "retro_tab5.h"

_Static_assert(sizeof(RETRO_TAB5_SD_MOUNT) > 1, "mount point");

static bool s_mounted;

bool retro_tab5_sd_mount(void)
{
    if (s_mounted) {
        return true;
    }
    if (strcmp(BSP_SD_MOUNT_POINT, RETRO_TAB5_SD_MOUNT) != 0) {
        RLOGE(SD, "CONFIG_BSP_SD_MOUNT_POINT is %s, expected %s", BSP_SD_MOUNT_POINT,
              RETRO_TAB5_SD_MOUNT);
        return false;
    }
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        RLOGW(SD, "mount failed: %s", esp_err_to_name(err));
        /* The BSP keeps the LDO handle on failure; release it so a retry
         * (card inserted later) can acquire it again. */
        bsp_sdcard_unmount();
        return false;
    }
    s_mounted = true;
    return true;
}

bool retro_tab5_sd_mounted(void)
{
    return s_mounted;
}

bool retro_tab5_sd_info(retro_tab5_sd_info_t *out)
{
    const sdmmc_card_t *card = bsp_sdcard_get_handle();
    if (!s_mounted || !card) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->name, card->cid.name, sizeof(card->cid.name));
    out->capacity_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    out->freq_khz = (unsigned)card->real_freq_khz;
    out->bus_width = 1u << card->log_bus_width;
    out->fs_type = "?";

    /* FatFs volume behind the mount: drive "<pdrv>:". f_getfree trusts the
     * FSINFO free count (CONFIG_FATFS_DONT_TRUST_FREE_CLUSTER_CNT=0), so it
     * doesn't scan the FAT. */
    char drv[8];
    snprintf(drv, sizeof(drv), "%u:", (unsigned)ff_diskio_get_pdrv_card(card));
    DWORD free_clusters;
    FATFS *fs;
    if (f_getfree(drv, &free_clusters, &fs) == FR_OK) {
        out->cluster_bytes = (unsigned)fs->csize * 512;
        out->fs_type = fs->fs_type == FS_EXFAT ? "exFAT" : fs->fs_type == FS_FAT32 ? "FAT32" : "FAT12/16";
    }
    return true;
}

bool retro_tab5_sd_read_sectors(void *buf, uint32_t first, size_t count)
{
    sdmmc_card_t *card = bsp_sdcard_get_handle();
    return s_mounted && card && sdmmc_read_sectors(card, buf, first, count) == ESP_OK;
}

void retro_tab5_sd_unmount(void)
{
    if (s_mounted) {
        bsp_sdcard_unmount();
        s_mounted = false;
    }
}

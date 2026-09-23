/*
 * Tab5 Retro Console: boot entry point.
 *
 * Phase 0: bring up logging and report the platform. Later phases add the
 * boot sequence (display, SD, library, launcher) and mode dispatch here.
 */
#include <inttypes.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"

#include "retro_log.h"

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
    }
}

static void log_platform(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    RLOGI(CORE, "Tab5 Retro Console %s (IDF %s, built %s %s)", app->version, app->idf_ver,
          app->date, app->time);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    RLOGI(CORE, "chip rev v%d.%d, %d cores, flash %" PRIu32 " MB", chip.revision / 100,
          chip.revision % 100, chip.cores, flash_size >> 20);

    RLOGI(CORE, "PSRAM %u KB; heap free: internal %u KB, PSRAM %u KB, DMA %u KB",
          (unsigned)(esp_psram_get_size() >> 10),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) >> 10));

    const esp_partition_t *running = esp_ota_get_running_partition();
    RLOGI(CORE, "running from %s @ 0x%" PRIx32 ", reset reason: %s",
          running ? running->label : "?", running ? running->address : 0,
          reset_reason_str(esp_reset_reason()));
}

void app_main(void)
{
    retro_log_init();
    log_platform();
    RLOGI(CORE, "boot complete");
}

#include "bsp/m5stack_tab5.h"

#include "retro_log.h"
#include "retro_tab5.h"

bool retro_tab5_usb_host_start(void)
{
    static bool s_started;
    if (s_started) {
        return true;
    }
    if (!retro_tab5_board_init()) {
        return false;
    }
    /* Arguments are ignored on the Tab5. Drives USB5V_EN (0x44 P3) high and
     * starts a "usb_lib" daemon task (priority 10, unpinned). */
    esp_err_t err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (err != ESP_OK) {
        RLOGE(USB, "USB host start failed: %s", esp_err_to_name(err));
        return false;
    }
    s_started = true;
    RLOGI(USB, "USB host running, USB-A VBUS on");
    return true;
}

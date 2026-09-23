/*
 * RetroHAL power and network on the Tab5: INA226 battery monitor, IO-expander
 * rails and the PWROFF_PLUSE power-off (plan §2a).
 */
#include "retro_battery.h"
#include "retro_log.h"
#include "retro_network.h"
#include "retro_power.h"
#include "retro_tab5.h"

/* Bring-up: before a pack was fitted the INA226 read the charger output
 * (4.3 to 8.4 V, wandering) at exactly 0 mA. A 2S pack never sits below
 * ~6 V, and a connected pack under load never reads exactly 0 mA. */
#define NO_BATTERY_MV 6000

static const retro_tab5_rail_t s_map[RETRO_RAIL_COUNT] = {
    [RETRO_RAIL_WIRELESS] = RETRO_TAB5_RAIL_WLAN,
    [RETRO_RAIL_USB_HOST] = RETRO_TAB5_RAIL_USB_A,
    [RETRO_RAIL_EXPANSION] = RETRO_TAB5_RAIL_EXT_5V,
    [RETRO_RAIL_SPEAKER] = RETRO_TAB5_RAIL_SPEAKER,
    [RETRO_RAIL_CHARGER] = RETRO_TAB5_RAIL_CHARGE,
};

bool retro_power_init(void)
{
    return retro_tab5_board_init();
}

bool retro_power_status(retro_power_status_t *out)
{
    *out = (retro_power_status_t){.percent = -1};
    retro_tab5_power_sample_t p;
    if (!retro_tab5_battery_read(&p)) {
        return false;
    }
    out->valid = true;
    out->battery_mv = p.bus_mv;
    out->current_ma = p.current_ma;
    out->power_mw = p.power_mw;
    /* The INA226 sits in the battery branch: on USB-C with a full or absent
     * pack it reads 0 mA, and without a pack the voltage is the charger's.
     * 0 mA above 8.3 V is taken as "no pack" (a full pack looks the same). */
    out->battery_present = p.bus_mv >= NO_BATTERY_MV && !(p.current_ma == 0 && p.bus_mv > 8300);
    out->charging = p.current_ma > 20;
    out->external_power = out->charging || p.current_ma == 0;
    if (out->battery_present) {
        out->percent = retro_battery_percent_2s(p.bus_mv, p.current_ma);
    }
    return true;
}

bool retro_power_rail_set(retro_rail_t rail, bool on)
{
    return rail < RETRO_RAIL_COUNT && retro_tab5_rail_set(s_map[rail], on);
}

int retro_power_rail_get(retro_rail_t rail)
{
    return rail < RETRO_RAIL_COUNT ? retro_tab5_rail_get(s_map[rail]) : -1;
}

const char *retro_power_rail_name(retro_rail_t rail)
{
    return rail < RETRO_RAIL_COUNT ? retro_tab5_rail_name(s_map[rail]) : "?";
}

void retro_power_off(void)
{
    RLOGI(POWER, "power off");
    retro_tab5_power_off();
}

bool retro_network_init(void)
{
    /* R19: the C6 stays powered down in v1. */
    return retro_tab5_rail_set(RETRO_TAB5_RAIL_WLAN, false);
}

retro_net_state_t retro_network_state(void)
{
    return retro_tab5_rail_get(RETRO_TAB5_RAIL_WLAN) == 1 ? RETRO_NET_DISCONNECTED : RETRO_NET_OFF;
}

bool retro_network_set_enabled(bool on)
{
    return retro_tab5_rail_set(RETRO_TAB5_RAIL_WLAN, on);
}

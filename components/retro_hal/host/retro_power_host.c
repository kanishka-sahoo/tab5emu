/*
 * Host power: always on mains, no battery. Rails only remember their state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "retro_log.h"
#include "retro_network.h"
#include "retro_power.h"

static int s_rails[RETRO_RAIL_COUNT] = {0, 1, 1, 0, 1};

static const char *const s_rail_names[RETRO_RAIL_COUNT] = {
    "wireless", "usb_host", "expansion", "speaker", "charger",
};

bool retro_power_init(void)
{
    return true;
}

bool retro_power_status(retro_power_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->external_power = true;
    out->percent = -1;
    return true;
}

bool retro_power_rail_set(retro_rail_t rail, bool on)
{
    if (rail >= RETRO_RAIL_COUNT) {
        return false;
    }
    s_rails[rail] = on;
    return true;
}

int retro_power_rail_get(retro_rail_t rail)
{
    return rail < RETRO_RAIL_COUNT ? s_rails[rail] : -1;
}

const char *retro_power_rail_name(retro_rail_t rail)
{
    return rail < RETRO_RAIL_COUNT ? s_rail_names[rail] : "?";
}

void retro_power_off(void)
{
    RLOGI(POWER, "power off requested (host: exiting)");
    exit(0);
}

bool retro_network_init(void)
{
    return true;
}

retro_net_state_t retro_network_state(void)
{
    return RETRO_NET_UNAVAILABLE;
}

bool retro_network_set_enabled(bool on)
{
    return !on;
}

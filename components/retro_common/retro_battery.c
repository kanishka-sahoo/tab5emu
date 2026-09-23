#include "retro_battery.h"

/* Typical Li-ion (LiCoO2 / NMC) open-circuit voltage per cell at rest. */
static const struct {
    int16_t mv;
    int8_t pct;
} s_curve[] = {
    {3300, 0}, {3500, 3}, {3600, 8}, {3680, 15}, {3730, 25}, {3780, 35}, {3820, 45},
    {3860, 55}, {3910, 65}, {3970, 75}, {4040, 85}, {4110, 93}, {4170, 98}, {4200, 100},
};

#define N (int)(sizeof(s_curve) / sizeof(s_curve[0]))

int retro_battery_percent_2s(int32_t pack_mv, int32_t current_ma)
{
    /* Discharging (current < 0) pulls the terminal voltage below open
     * circuit; charging pushes it above. Undo both. */
    int32_t ocv = pack_mv - current_ma * RETRO_BATTERY_2S_RESISTANCE_MOHM / 1000;
    int32_t cell = ocv / 2;
    if (cell <= s_curve[0].mv) {
        return 0;
    }
    if (cell >= s_curve[N - 1].mv) {
        return 100;
    }
    for (int i = 1; i < N; i++) {
        if (cell < s_curve[i].mv) {
            int32_t v0 = s_curve[i - 1].mv, v1 = s_curve[i].mv;
            int32_t p0 = s_curve[i - 1].pct, p1 = s_curve[i].pct;
            return (int)(p0 + (cell - v0) * (p1 - p0) / (v1 - v0));
        }
    }
    return 100;
}

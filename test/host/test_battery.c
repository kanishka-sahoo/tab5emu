#include "retro_battery.h"
#include "test_util.h"

static void test_clamps(void)
{
    CHECK(retro_battery_percent_2s(0, 0) == 0);
    CHECK(retro_battery_percent_2s(6000, 0) == 0);
    CHECK(retro_battery_percent_2s(6600, 0) == 0); /* 3.30 V per cell */
    CHECK(retro_battery_percent_2s(8400, 0) == 100); /* 4.20 V per cell */
    CHECK(retro_battery_percent_2s(9000, 0) == 100);
}

static void test_curve_points(void)
{
    CHECK(retro_battery_percent_2s(2 * 3820, 0) == 45);
    CHECK(retro_battery_percent_2s(2 * 4040, 0) == 85);
    /* Halfway between 3600 (8%) and 3680 (15%). */
    CHECK(retro_battery_percent_2s(2 * 3640, 0) == 11);
}

static void test_monotonic(void)
{
    int prev = 0, rises = 0;
    for (int32_t mv = 6000; mv <= 8600; mv += 2) {
        int p = retro_battery_percent_2s(mv, 0);
        CHECK(p >= 0 && p <= 100);
        CHECK(p >= prev);
        rises += p > prev;
        prev = p;
    }
    CHECK(prev == 100);
    CHECK(rises > 50); /* actually climbs, not a step */
}

static void test_current_compensation(void)
{
    const int32_t mv = 7600; /* mid curve */
    int rest = retro_battery_percent_2s(mv, 0);
    /* Discharging sags the terminal voltage: the true charge is higher. */
    CHECK(retro_battery_percent_2s(mv, -2000) > rest);
    /* Charging lifts it: the true charge is lower. */
    CHECK(retro_battery_percent_2s(mv, 2000) < rest);
    /* 2 A through 150 mOhm is 300 mV on the pack. */
    CHECK(retro_battery_percent_2s(mv, -2000) == retro_battery_percent_2s(mv + 300, 0));
}

int main(void)
{
    RUN(test_clamps);
    RUN(test_curve_points);
    RUN(test_monotonic);
    RUN(test_current_compensation);
    return TEST_EXIT();
}

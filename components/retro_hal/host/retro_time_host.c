#include <time.h>

#include "retro_time.h"

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t retro_time_us(void)
{
    static uint64_t start;
    if (start == 0) {
        start = now_us();
    }
    return now_us() - start;
}

bool retro_time_wall_get(struct tm *out)
{
    time_t now = time(NULL);
    return localtime_r(&now, out) != NULL;
}

bool retro_time_wall_set(const struct tm *tm)
{
    (void)tm;
    return false;
}

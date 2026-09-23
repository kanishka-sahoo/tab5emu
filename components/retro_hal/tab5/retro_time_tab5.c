#include "esp_timer.h"

#include "retro_time.h"

uint64_t retro_time_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

/*
 * RetroHAL platform on the Tab5: board bring-up. There is no event loop to
 * pump; input and display run on their own tasks.
 */
#include "retro_platform.h"
#include "retro_tab5.h"

bool retro_platform_init(void)
{
    return retro_tab5_board_init();
}

bool retro_platform_pump(void)
{
    return true;
}

void retro_platform_deinit(void)
{
}

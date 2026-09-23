#include <pthread.h>
#include <stdio.h>

#include "retro_log_port.h"
#include "retro_time.h"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void retro_log_port_init(void)
{
}

void retro_log_port_lock(void)
{
    pthread_mutex_lock(&s_lock);
}

void retro_log_port_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

unsigned long retro_log_port_time_ms(void)
{
    return retro_time_ms();
}

void retro_log_port_console(retro_log_level_t level, const char *line, size_t len)
{
    (void)level;
    fwrite(line, 1, len, stderr);
}

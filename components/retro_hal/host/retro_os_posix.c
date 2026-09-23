/*
 * RetroHAL OS services on POSIX threads (host build). Priorities and core
 * affinity are ignored.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "retro_log.h"
#include "retro_os.h"

struct retro_task {
    pthread_t thread;
    void (*fn)(void *);
    void *arg;
};

struct retro_mutex {
    pthread_mutex_t m;
};

struct retro_sem {
    pthread_mutex_t m;
    pthread_cond_t c;
    unsigned count, max;
};

static void *task_main(void *p)
{
    retro_task_t *t = p;
    t->fn(t->arg);
    return NULL;
}

retro_task_t *retro_task_create(const retro_task_config_t *cfg)
{
    retro_task_t *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->fn = cfg->fn;
    t->arg = cfg->arg;
    if (pthread_create(&t->thread, NULL, task_main, t) != 0) {
        RLOGE(CORE, "can't start task %s", cfg->name);
        free(t);
        return NULL;
    }
    return t;
}

void retro_task_join(retro_task_t *t)
{
    if (t) {
        pthread_join(t->thread, NULL);
        free(t);
    }
}

void retro_sleep_us(uint32_t us)
{
    struct timespec ts = {(time_t)(us / 1000000u), (long)(us % 1000000u) * 1000};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

void retro_sleep_ms(uint32_t ms)
{
    retro_sleep_us(ms * 1000u);
}

void retro_yield(void)
{
    sched_yield();
}

retro_mutex_t *retro_mutex_create(void)
{
    retro_mutex_t *m = calloc(1, sizeof(*m));
    if (m) {
        pthread_mutex_init(&m->m, NULL);
    }
    return m;
}

void retro_mutex_destroy(retro_mutex_t *m)
{
    if (m) {
        pthread_mutex_destroy(&m->m);
        free(m);
    }
}

void retro_mutex_lock(retro_mutex_t *m)
{
    pthread_mutex_lock(&m->m);
}

void retro_mutex_unlock(retro_mutex_t *m)
{
    pthread_mutex_unlock(&m->m);
}

retro_sem_t *retro_sem_create(unsigned max, unsigned initial)
{
    retro_sem_t *s = calloc(1, sizeof(*s));
    if (s) {
        pthread_mutex_init(&s->m, NULL);
        pthread_cond_init(&s->c, NULL);
        s->max = max ? max : 1;
        s->count = initial > s->max ? s->max : initial;
    }
    return s;
}

void retro_sem_destroy(retro_sem_t *s)
{
    if (s) {
        pthread_cond_destroy(&s->c);
        pthread_mutex_destroy(&s->m);
        free(s);
    }
}

void retro_sem_give(retro_sem_t *s)
{
    pthread_mutex_lock(&s->m);
    if (s->count < s->max) {
        s->count++;
    }
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}

bool retro_sem_take(retro_sem_t *s, uint32_t timeout_ms)
{
    pthread_mutex_lock(&s->m);
    if (timeout_ms == RETRO_WAIT_FOREVER) {
        while (s->count == 0) {
            pthread_cond_wait(&s->c, &s->m);
        }
    } else {
        struct timeval now;
        gettimeofday(&now, NULL);
        uint64_t ns = (uint64_t)now.tv_usec * 1000u + (uint64_t)timeout_ms * 1000000u;
        struct timespec until = {now.tv_sec + (time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
        while (s->count == 0) {
            if (pthread_cond_timedwait(&s->c, &s->m, &until) == ETIMEDOUT) {
                break;
            }
        }
    }
    bool ok = s->count > 0;
    if (ok) {
        s->count--;
    }
    pthread_mutex_unlock(&s->m);
    return ok;
}

#define HOST_ALIGN 128

void *retro_mem_alloc(size_t bytes, unsigned caps)
{
    (void)caps;
    void *p = NULL;
    size_t rounded = (bytes + HOST_ALIGN - 1) & ~(size_t)(HOST_ALIGN - 1);
    if (posix_memalign(&p, HOST_ALIGN, rounded ? rounded : HOST_ALIGN) != 0) {
        return NULL;
    }
    memset(p, 0, rounded);
    return p;
}

void retro_mem_free(void *p)
{
    free(p);
}

bool retro_mem_is_internal(const void *p)
{
    (void)p;
    return true;
}

void retro_heap_info(retro_heap_info_t *out)
{
    memset(out, 0, sizeof(*out));
}

int retro_cpu_load(int pct[RETRO_MAX_CPUS])
{
    for (int i = 0; i < RETRO_MAX_CPUS; i++) {
        pct[i] = -1;
    }
    return RETRO_MAX_CPUS;
}

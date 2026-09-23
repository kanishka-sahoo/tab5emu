/*
 * RetroHAL OS services: tasks, locks, semaphores, memory placement and
 * system load. FROZEN (plan Phase 2).
 *
 * Lets the frontend pipelines (video, audio, input) run unchanged on the
 * Tab5 (FreeRTOS) and in the host build (pthreads), so they can be unit
 * tested on the desktop (plan D8).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Tasks ------------------------------------------------------------------------ */

typedef struct retro_task retro_task_t;

#define RETRO_CORE_ANY (-1)

typedef struct {
    const char *name;
    void (*fn)(void *arg);
    void *arg;
    size_t stack_bytes;
    int priority; /* FreeRTOS priority on the Tab5 (plan §3.2); ignored on the host */
    int core;     /* 0, 1 or RETRO_CORE_ANY */
} retro_task_config_t;

/* Start a task. It ends when fn returns. NULL on failure. */
retro_task_t *retro_task_create(const retro_task_config_t *cfg);

/* Wait for the task's function to return, then free the handle. */
void retro_task_join(retro_task_t *t);

void retro_sleep_ms(uint32_t ms);
void retro_sleep_us(uint32_t us);

/* Let other tasks of the same priority run. */
void retro_yield(void);

/* ---- Locks and semaphores ----------------------------------------------------------- */

typedef struct retro_mutex retro_mutex_t;
typedef struct retro_sem retro_sem_t;

#define RETRO_WAIT_FOREVER UINT32_MAX

retro_mutex_t *retro_mutex_create(void);
void retro_mutex_destroy(retro_mutex_t *m);
void retro_mutex_lock(retro_mutex_t *m);
void retro_mutex_unlock(retro_mutex_t *m);

/* Counting semaphore, saturating at max. A binary semaphore is max = 1. */
retro_sem_t *retro_sem_create(unsigned max, unsigned initial);
void retro_sem_destroy(retro_sem_t *s);
void retro_sem_give(retro_sem_t *s);
/* Returns false on timeout. */
bool retro_sem_take(retro_sem_t *s, uint32_t timeout_ms);

/* ---- Memory -------------------------------------------------------------------------- */

typedef enum {
    RETRO_MEM_ANY = 0,
    /* On-chip SRAM: emulator hot state, native frame buffers (plan §3.3).
     * Falls back to PSRAM when combined with RETRO_MEM_FALLBACK. */
    RETRO_MEM_INTERNAL = 1u << 0,
    RETRO_MEM_PSRAM = 1u << 1,
    RETRO_MEM_DMA = 1u << 2,
    RETRO_MEM_FALLBACK = 1u << 3,
} retro_mem_caps_t;

/* Aligned to at least the largest cache line (so DMA engines and cache
 * maintenance never share a line with other data). Zeroed. */
void *retro_mem_alloc(size_t bytes, unsigned caps);
void retro_mem_free(void *p);

/* True if p is in on-chip SRAM (always true on the host). */
bool retro_mem_is_internal(const void *p);

typedef struct {
    size_t internal_free, internal_min_free, internal_largest;
    size_t psram_free, psram_total;
    size_t dma_free;
} retro_heap_info_t;

void retro_heap_info(retro_heap_info_t *out);

/* ---- Load ------------------------------------------------------------------------------ */

#define RETRO_MAX_CPUS 2

/* Busy percentage per CPU since the previous call (0..100), -1 where not
 * available (instrumentation disabled, or the host). Returns the CPU count. */
int retro_cpu_load(int pct[RETRO_MAX_CPUS]);

#ifdef __cplusplus
}
#endif

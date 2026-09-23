/*
 * RetroHAL OS services on FreeRTOS (Tab5).
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "retro_log.h"
#include "retro_os.h"

/* PPA/DMA buffers and cache maintenance need the L2 line (128 B). */
#define MEM_ALIGN 128

struct retro_task {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
};

static void task_main(void *p)
{
    retro_task_t *t = p;
    t->fn(t->arg);
    xSemaphoreGive(t->done);
    vTaskDelete(NULL);
}

retro_task_t *retro_task_create(const retro_task_config_t *cfg)
{
    retro_task_t *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->fn = cfg->fn;
    t->arg = cfg->arg;
    t->done = xSemaphoreCreateBinary();
    const BaseType_t core = cfg->core == RETRO_CORE_ANY ? tskNO_AFFINITY : cfg->core;
    if (!t->done || xTaskCreatePinnedToCore(task_main, cfg->name, cfg->stack_bytes, t,
                                            (UBaseType_t)cfg->priority, NULL, core) != pdPASS) {
        RLOGE(CORE, "can't start task %s", cfg->name);
        if (t->done) {
            vSemaphoreDelete(t->done);
        }
        free(t);
        return NULL;
    }
    return t;
}

void retro_task_join(retro_task_t *t)
{
    if (t) {
        xSemaphoreTake(t->done, portMAX_DELAY);
        vSemaphoreDelete(t->done);
        free(t);
    }
}

void retro_sleep_ms(uint32_t ms)
{
    vTaskDelay(ms ? pdMS_TO_TICKS(ms) : 0);
}

void retro_sleep_us(uint32_t us)
{
    if (us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
        us %= 1000;
    }
    if (us) {
        esp_rom_delay_us(us);
    }
}

void retro_yield(void)
{
    taskYIELD();
}

static TickType_t ticks(uint32_t ms)
{
    return ms == RETRO_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

retro_mutex_t *retro_mutex_create(void)
{
    return (retro_mutex_t *)xSemaphoreCreateMutex();
}

void retro_mutex_destroy(retro_mutex_t *m)
{
    if (m) {
        vSemaphoreDelete((SemaphoreHandle_t)m);
    }
}

void retro_mutex_lock(retro_mutex_t *m)
{
    xSemaphoreTake((SemaphoreHandle_t)m, portMAX_DELAY);
}

void retro_mutex_unlock(retro_mutex_t *m)
{
    xSemaphoreGive((SemaphoreHandle_t)m);
}

retro_sem_t *retro_sem_create(unsigned max, unsigned initial)
{
    return (retro_sem_t *)xSemaphoreCreateCounting(max ? max : 1, initial);
}

void retro_sem_destroy(retro_sem_t *s)
{
    if (s) {
        vSemaphoreDelete((SemaphoreHandle_t)s);
    }
}

void retro_sem_give(retro_sem_t *s)
{
    xSemaphoreGive((SemaphoreHandle_t)s);
}

bool retro_sem_take(retro_sem_t *s, uint32_t timeout_ms)
{
    return xSemaphoreTake((SemaphoreHandle_t)s, ticks(timeout_ms)) == pdTRUE;
}

static uint32_t heap_caps(unsigned caps)
{
    uint32_t c = MALLOC_CAP_8BIT;
    if (caps & RETRO_MEM_INTERNAL) {
        c |= MALLOC_CAP_INTERNAL;
    }
    if (caps & RETRO_MEM_PSRAM) {
        c |= MALLOC_CAP_SPIRAM;
    }
    if (caps & RETRO_MEM_DMA) {
        c |= MALLOC_CAP_DMA;
    }
    return c;
}

void *retro_mem_alloc(size_t bytes, unsigned caps)
{
    size_t rounded = (bytes + MEM_ALIGN - 1) & ~(size_t)(MEM_ALIGN - 1);
    if (rounded == 0) {
        rounded = MEM_ALIGN;
    }
    void *p = heap_caps_aligned_calloc(MEM_ALIGN, 1, rounded, heap_caps(caps));
    if (!p && (caps & RETRO_MEM_FALLBACK)) {
        p = heap_caps_aligned_calloc(MEM_ALIGN, 1, rounded, MALLOC_CAP_8BIT);
    }
    return p;
}

void retro_mem_free(void *p)
{
    heap_caps_free(p);
}

bool retro_mem_is_internal(const void *p)
{
    return esp_ptr_internal(p);
}

void retro_heap_info(retro_heap_info_t *out)
{
    out->internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->internal_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
}

int retro_cpu_load(int pct[RETRO_MAX_CPUS])
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    /* Busy = 1 - idle-task share of wall time since the previous call.
     * The counters are esp_timer microseconds (32-bit: deltas wrap
     * correctly for intervals under 71 minutes). */
    static configRUN_TIME_COUNTER_TYPE s_idle[RETRO_MAX_CPUS];
    static configRUN_TIME_COUNTER_TYPE s_wall;
    configRUN_TIME_COUNTER_TYPE wall = (configRUN_TIME_COUNTER_TYPE)esp_timer_get_time();
    configRUN_TIME_COUNTER_TYPE dt = wall - s_wall;
    for (int c = 0; c < RETRO_MAX_CPUS; c++) {
        configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(c);
        configRUN_TIME_COUNTER_TYPE di = idle - s_idle[c];
        s_idle[c] = idle;
        int busy = (s_wall && dt) ? 100 - (int)((uint64_t)di * 100 / dt) : -1;
        pct[c] = busy < 0 && s_wall ? 0 : busy > 100 ? 100 : busy;
    }
    s_wall = wall;
#else
    for (int c = 0; c < RETRO_MAX_CPUS; c++) {
        pct[c] = -1;
    }
#endif
    return RETRO_MAX_CPUS;
}

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble_nimble_host.h"
#include "host_freertos.h"
#include "host/ble_hs.h"

static StaticTask_t s_host_task;
static StaticTask_t s_stop_task;
static StaticSemaphore_t s_semaphores[2];
static atomic_uint s_sem_next;
static atomic_uint s_blocks;
static atomic_uint s_stop_calls;
static atomic_int s_stop_result;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_changed = PTHREAD_COND_INITIALIZER;
static bool s_block_stop;
static bool s_stop_entered;
static bool s_exit_requested;
static bool s_block_exit;

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return xSemaphoreCreateBinaryStatic(&s_semaphores[atomic_fetch_add(&s_sem_next, 1U) % 2U]);
}
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
                                   uint32_t depth, void *arg, UBaseType_t priority,
                                   TaskHandle_t *task, BaseType_t core)
{
    static StackType_t stack[1];
    const bool is_host = strcmp(name, "nimble_host") == 0;

    if (is_host)
    {
        pthread_mutex_lock(&s_lock);
        s_exit_requested = false;
        pthread_mutex_unlock(&s_lock);
    }
    *task = xTaskCreateStaticPinnedToCore(entry, name, depth, arg, priority,
                                          stack, is_host ? &s_host_task : &s_stop_task,
                                          core);
    return *task != NULL ? pdPASS : pdFAIL;
}
void nimble_port_run(void)
{
    pthread_mutex_lock(&s_lock);
    while (!s_exit_requested || s_block_exit) pthread_cond_wait(&s_changed, &s_lock);
    pthread_mutex_unlock(&s_lock);
}
int nimble_port_stop(void)
{
    atomic_fetch_add(&s_stop_calls, 1U);
    pthread_mutex_lock(&s_lock);
    s_stop_entered = true;
    pthread_cond_broadcast(&s_changed);
    while (s_block_stop) pthread_cond_wait(&s_changed, &s_lock);
    const int result = atomic_load(&s_stop_result);
    if (result == 0)
    {
        s_exit_requested = true;
        pthread_cond_broadcast(&s_changed);
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
void ble_npl_event_init(struct ble_npl_event *event, ble_npl_event_fn *fn, void *arg)
{
    (void)arg;
    /* Same allocation contract as the selected SDK: init reuses non-NULL
     * backing; deinit is responsible for returning it and clearing it. */
    assert(event->event == NULL);
    event->event = malloc(sizeof(fn));
    assert(event->event != NULL);
    *(ble_npl_event_fn **)event->event = fn;
    atomic_fetch_add(&s_blocks, 1U);
}
void ble_npl_event_deinit(struct ble_npl_event *event)
{
    assert(event->event != NULL);
    free(event->event);
    event->event = NULL;
    atomic_fetch_sub(&s_blocks, 1U);
}
static void _event(struct ble_npl_event *event)
{
    (void)event;
}
static void _init(ble_nimble_host_t *host)
{
    ble_nimble_host_init_events(host, _event, _event, _event);
    assert(atomic_load(&s_blocks) == 3U);
    assert(ble_nimble_host_start(host, 4096U, 0) == ESP_OK);
}
static void _release(ble_nimble_host_t *host)
{
    assert(ble_nimble_host_release(host) == ESP_OK);
    assert(ble_nimble_host_release(host) == ESP_OK);
    assert(atomic_load(&s_blocks) == 0U);
    assert(host_freertos_live_tasks() == 0U);
    assert(host_freertos_live_semaphores() == 0U);
}
static void test_restart_and_partial_init(void)
{
    ble_nimble_host_t host = {0};
    for (unsigned i = 0U; i < 50U; ++i)
    {
        _init(&host);
        assert(ble_nimble_host_stop(&host, 1000U, 0) == ESP_OK);
        _release(&host);
    }
    ble_nimble_host_init_events(&host, _event, _event, _event);
    _release(&host); /* Failure before host creation still balances events. */

    ble_nimble_host_init_events(&host, _event, _event, _event);
    host_freertos_fail_next_task_creates(1U);
    assert(ble_nimble_host_start(&host, 4096U, 0) == ESP_ERR_NO_MEM);
    assert(!host.started);
    _release(&host);
}
static void test_stop_timeout_waits_same_worker(void)
{
    ble_nimble_host_t host = {0};
    _init(&host);
    const unsigned stops = atomic_load(&s_stop_calls);
    pthread_mutex_lock(&s_lock);
    s_block_stop = true;
    s_stop_entered = false;
    pthread_mutex_unlock(&s_lock);
    assert(ble_nimble_host_stop(&host, 5U, 0) == ESP_ERR_TIMEOUT);
    assert(host.started && host.stop_task != NULL);
    assert(ble_nimble_host_release(&host) == ESP_ERR_INVALID_STATE);
    assert(ble_nimble_host_start(&host, 4096U, 0) == ESP_ERR_INVALID_STATE);
    assert(ble_nimble_host_stop(&host, 5U, 0) == ESP_ERR_TIMEOUT);
    pthread_mutex_lock(&s_lock);
    assert(s_stop_entered);
    s_block_stop = false;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
    assert(ble_nimble_host_stop(&host, 1000U, 0) == ESP_OK);
    assert(atomic_load(&s_stop_calls) == stops + 1U);
    _release(&host);
}
static void test_failed_stop_never_deletes_live_host(void)
{
    ble_nimble_host_t host = {0};
    _init(&host);
    host_freertos_fail_next_task_creates(1U);
    assert(ble_nimble_host_stop(&host, 1000U, 0) == ESP_ERR_NO_MEM);
    assert(host.started && host_freertos_live_tasks() == 1U);
    atomic_store(&s_stop_result, 12);
    assert(ble_nimble_host_stop(&host, 1000U, 0) == ESP_FAIL);
    assert(host.started);
    assert(host_freertos_live_tasks() == 1U);
    atomic_store(&s_stop_result, 0);
    assert(ble_nimble_host_stop(&host, 1000U, 0) == ESP_OK);
    _release(&host);
}
static void test_stop_result_does_not_prove_run_exit(void)
{
    ble_nimble_host_t host = {0};
    _init(&host);
    const unsigned stops = atomic_load(&s_stop_calls);
    pthread_mutex_lock(&s_lock);
    s_block_exit = true;
    pthread_mutex_unlock(&s_lock);
    assert(ble_nimble_host_stop(&host, 5U, 0) == ESP_ERR_TIMEOUT);
    assert(host.started && host.stop_confirmed);
    pthread_mutex_lock(&s_lock);
    s_block_exit = false;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
    assert(ble_nimble_host_stop(&host, 1000U, 0) == ESP_OK);
    assert(atomic_load(&s_stop_calls) == stops + 1U);
    _release(&host);

    _init(&host);
    atomic_store(&s_stop_result, BLE_HS_EALREADY);
    assert(ble_nimble_host_stop(&host, 5U, 0) == ESP_ERR_TIMEOUT);
    assert(host.started);
    pthread_mutex_lock(&s_lock);
    s_exit_requested = true;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
    assert(ble_nimble_host_stop(&host, 1000U, 0) == ESP_OK);
    atomic_store(&s_stop_result, 0);
    _release(&host);
}
int main(void)
{
    test_restart_and_partial_init();
    test_stop_timeout_waits_same_worker();
    test_failed_stop_never_deletes_live_host();
    test_stop_result_does_not_prove_run_exit();
    puts("host events and confirmed stop lifecycle pass");
    return 0;
}

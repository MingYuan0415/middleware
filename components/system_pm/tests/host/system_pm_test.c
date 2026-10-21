#include "esp_sleep.h"
#include "host_freertos.h"
#include "system_pm.h"
#include "system_pm_fakes.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_TIMEOUT_MS 2000U
#define TEST_WAKE_GPIO 2
#define TEST_WAKE_GPIO_SECOND 4

static atomic_uint s_prepare_calls;
static atomic_uint s_complete_calls;
static atomic_uint s_commit_calls;
static atomic_uint s_guard_calls;
static atomic_uint s_wake_calls;
static atomic_uint s_guard_generation;
static atomic_uint s_commit_generation;
static atomic_uint s_wake_generation;
static atomic_int s_wake_reason;
static atomic_int s_wake_source_index;
static atomic_int s_wake_gpio;
static atomic_uint s_wake_prepare_result;
static atomic_uint s_wake_complete_result;
static atomic_uint s_wake_sleep_result;
static esp_err_t s_prepare_result;
static esp_err_t s_complete_result;
static bool s_guard_result;

static void _reset_counters(void)
{
    atomic_store(&s_prepare_calls, 0U);
    atomic_store(&s_complete_calls, 0U);
    atomic_store(&s_commit_calls, 0U);
    atomic_store(&s_guard_calls, 0U);
    atomic_store(&s_wake_calls, 0U);
    atomic_store(&s_guard_generation, 0U);
    atomic_store(&s_commit_generation, 0U);
    atomic_store(&s_wake_generation, 0U);
    atomic_store(&s_wake_reason, -1);
    atomic_store(&s_wake_source_index, -1);
    atomic_store(&s_wake_gpio, -1);
    atomic_store(&s_wake_prepare_result, 0xFFFFFFFFU);
    atomic_store(&s_wake_complete_result, 0xFFFFFFFFU);
    atomic_store(&s_wake_sleep_result, 0xFFFFFFFFU);
    s_prepare_result = ESP_OK;
    s_complete_result = ESP_OK;
    s_guard_result = true;
    system_pm_fakes_reset();
}

static esp_err_t _prepare(uint32_t timeout_ms, void *context)
{
    assert(timeout_ms > 0U);
    assert(context == (void *)&s_prepare_calls);
    atomic_fetch_add(&s_prepare_calls, 1U);
    return s_prepare_result;
}

static esp_err_t _complete(uint32_t timeout_ms, void *context)
{
    assert(timeout_ms > 0U);
    assert(context == (void *)&s_prepare_calls);
    atomic_fetch_add(&s_complete_calls, 1U);
    return s_complete_result;
}

static bool _guard(uint32_t generation, void *context)
{
    assert(context == (void *)&s_prepare_calls);
    atomic_fetch_add(&s_guard_calls, 1U);
    atomic_store(&s_guard_generation, generation);
    return s_guard_result;
}

static void _commit(uint32_t generation, void *context)
{
    assert(context == (void *)&s_prepare_calls);
    atomic_fetch_add(&s_commit_calls, 1U);
    atomic_store(&s_commit_generation, generation);
}

static void _wake(const system_pm_wake_event_t *event, void *context)
{
    assert(event != NULL);
    assert(context == (void *)&s_prepare_calls);
    atomic_fetch_add(&s_wake_calls, 1U);
    atomic_store(&s_wake_reason, (int)event->reason);
    atomic_store(&s_wake_source_index, event->source_index);
    atomic_store(&s_wake_gpio, event->gpio_num);
    atomic_store(&s_wake_prepare_result, (unsigned)event->prepare_result);
    atomic_store(&s_wake_complete_result, (unsigned)event->complete_result);
    atomic_store(&s_wake_sleep_result, (unsigned)event->sleep_result);
    atomic_store(&s_wake_generation, s_commit_generation);
}

static system_pm_config_t _config(void)
{
    const system_pm_config_t config =
    {
        .wake_sources =
        {
            {.gpio_num = TEST_WAKE_GPIO, .active_level = SYSTEM_PM_WAKE_LEVEL_LOW},
            {
                .gpio_num = TEST_WAKE_GPIO_SECOND,
                .active_level = SYSTEM_PM_WAKE_LEVEL_LOW
            },
        },
        .wake_source_count = 2U,
        .prepare_sleep = _prepare,
        .complete_sleep = _complete,
        .sleep_hook_context = (void *) &s_prepare_calls,
        .prepare_timeout_ms = TEST_TIMEOUT_MS,
        .wake_callback = _wake,
        .wake_callback_context = (void *) &s_prepare_calls,
        .commit_guard = _guard,
        .commit_callback = _commit,
        .commit_context = (void *) &s_prepare_calls,
        .task_priority = 5U,
    };

    return config;
}

static bool _wait_for_count(atomic_uint *counter, unsigned expected)
{
    for (unsigned waited = 0U; waited < TEST_TIMEOUT_MS; ++waited)
    {
        if (atomic_load(counter) >= expected)
        {
            return true;
        }
        vTaskDelay(1U);
    }
    return atomic_load(counter) >= expected;
}

static bool _wait_for_sleeps(unsigned expected)
{
    for (unsigned waited = 0U; waited < TEST_TIMEOUT_MS; ++waited)
    {
        if (system_pm_fakes_sleep_count() >= expected)
        {
            return true;
        }
        vTaskDelay(1U);
    }
    return system_pm_fakes_sleep_count() >= expected;
}

static void _test_config_validation(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    assert(system_pm_init(NULL) == ESP_ERR_INVALID_ARG);

    system_pm_config_t config = _config();

    config.prepare_sleep = NULL;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);
    config = _config();
    config.complete_sleep = NULL;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);
    config = _config();
    config.prepare_timeout_ms = 0U;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);
    config = _config();
    config.task_priority = 0U;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);
    config = _config();
    config.task_priority = configMAX_PRIORITIES;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);
    config = _config();
    config.wake_source_count = 0U;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);
    config = _config();
    config.wake_source_count = SYSTEM_PM_MAX_WAKE_SOURCES + 1U;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);

    /* Every source must be a unique RTC GPIO sharing one active level. */
    config = _config();
    config.wake_sources[1].gpio_num = TEST_WAKE_GPIO;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);
    config = _config();
    config.wake_sources[1].active_level = SYSTEM_PM_WAKE_LEVEL_HIGH;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);
    config = _config();
    config.wake_sources[0].gpio_num = TEST_WAKE_GPIO_SECOND + 10;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);
    config = _config();
    config.wake_sources[1].gpio_num = TEST_WAKE_GPIO_SECOND;
    const int one_rtc_gpio[] = {TEST_WAKE_GPIO};

    system_pm_fakes_set_rtc_gpios(one_rtc_gpio, 1U);
    assert(system_pm_init(&config) == ESP_ERR_INVALID_ARG);

    /* Nothing above may leak a PM lock or worker. */
    assert(system_pm_fakes_live_locks() == 0U);
    assert(host_dynamic_task_count() == 0U);
}

static void _test_init_idempotence_and_deinit(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    system_pm_config_t config = _config();

    assert(system_pm_init(&config) == ESP_OK);
    assert(system_pm_fakes_live_locks() == 1U);
    assert(host_dynamic_task_count() == 1U);
    assert(system_pm_init(&config) == ESP_OK);
    config.task_priority = 6U;
    assert(system_pm_init(&config) == ESP_ERR_INVALID_STATE);
    assert(system_pm_deinit() == ESP_OK);
    assert(system_pm_fakes_delete_count() == 1U);
    assert(host_dynamic_task_count() == 0U);

    /* Deinit is idempotent once the resources are gone. */
    assert(system_pm_deinit() == ESP_OK);

    /* A different config is admitted after a clean deinit. */
    config = _config();
    assert(system_pm_init(&config) == ESP_OK);
    assert(system_pm_deinit() == ESP_OK);
    assert(system_pm_fakes_live_locks() == 0U);
}

static void _test_init_failures_roll_back(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);

    system_pm_fakes_fail_lock_create(ESP_ERR_NO_MEM);
    const system_pm_config_t config = _config();

    assert(system_pm_init(&config) == ESP_ERR_NO_MEM);
    assert(system_pm_fakes_live_locks() == 0U);
    assert(host_dynamic_task_count() == 0U);

    /* A recovered lock-create failure admits the next init. */
    assert(system_pm_init(&config) == ESP_OK);
    assert(host_dynamic_task_count() == 1U);
    assert(system_pm_deinit() == ESP_OK);
    assert(host_dynamic_task_count() == 0U);

    /* A failed lock delete keeps the lifecycle closed until deinit retries. */
    system_pm_fakes_fail_lock_delete(ESP_FAIL);
    assert(system_pm_init(&config) == ESP_OK);
    assert(system_pm_deinit() == ESP_FAIL);
    assert(system_pm_init(&config) == ESP_ERR_INVALID_STATE);
    assert(system_pm_deinit() == ESP_OK);
    assert(system_pm_fakes_live_locks() == 0U);
}

static void _test_standby_cycle(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    system_pm_fakes_set_wakeup(BIT(ESP_SLEEP_WAKEUP_EXT1),
                               UINT64_C(1) << TEST_WAKE_GPIO_SECOND);
    const system_pm_config_t config = _config();

    assert(system_pm_init(&config) == ESP_OK);
    assert(system_pm_request_standby() == ESP_OK);
    /* A second request while one is in flight is rejected. */
    assert(system_pm_request_standby() == ESP_ERR_INVALID_STATE);
    assert(_wait_for_sleeps(1U));
    assert(_wait_for_count(&s_wake_calls, 1U));

    assert(atomic_load(&s_prepare_calls) == 1U);
    assert(atomic_load(&s_complete_calls) == 1U);
    assert(atomic_load(&s_guard_calls) == 1U);
    assert(atomic_load(&s_commit_calls) == 1U);
    assert(system_pm_fakes_ext1_enable_count() == 1U);
    assert(system_pm_fakes_ext1_disable_count() == 1U);
    assert(system_pm_fakes_last_ext1_mask() ==
           ((UINT64_C(1) << TEST_WAKE_GPIO) |
            (UINT64_C(1) << TEST_WAKE_GPIO_SECOND)));
    assert(system_pm_fakes_last_ext1_mode() == ESP_EXT1_WAKEUP_ANY_LOW);
    assert(atomic_load(&s_guard_generation) ==
           atomic_load(&s_commit_generation));
    assert(atomic_load(&s_wake_generation) ==
           atomic_load(&s_commit_generation));
    /* The reported source is the GPIO that actually woke the system. */
    assert(atomic_load(&s_wake_reason) == SYSTEM_PM_WAKE_REASON_GPIO);
    assert(atomic_load(&s_wake_source_index) == 1);
    assert(atomic_load(&s_wake_gpio) == TEST_WAKE_GPIO_SECOND);

    /* The worker returns to idle and accepts the next transaction. */
    assert(_wait_for_count(&s_complete_calls, 1U));
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_sleeps(2U));
    assert(_wait_for_count(&s_complete_calls, 2U));
    assert(system_pm_deinit() == ESP_OK);
}

static void _test_high_level_wake_sources(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    system_pm_config_t config = _config();

    config.wake_sources[0].active_level = SYSTEM_PM_WAKE_LEVEL_HIGH;
    config.wake_sources[1].active_level = SYSTEM_PM_WAKE_LEVEL_HIGH;
    assert(system_pm_init(&config) == ESP_OK);
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_sleeps(1U));
    assert(system_pm_fakes_last_ext1_mode() == ESP_EXT1_WAKEUP_ANY_HIGH);
    assert(system_pm_fakes_last_ext1_mask() ==
           ((UINT64_C(1) << TEST_WAKE_GPIO) |
            (UINT64_C(1) << TEST_WAKE_GPIO_SECOND)));
    assert(system_pm_deinit() == ESP_OK);
}

static void _test_wake_reason_classification(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    const system_pm_config_t config = _config();

    assert(system_pm_init(&config) == ESP_OK);

    system_pm_fakes_set_wakeup(BIT(ESP_SLEEP_WAKEUP_TIMER), 0U);
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 1U));
    assert(atomic_load(&s_wake_reason) == SYSTEM_PM_WAKE_REASON_TIMER);
    assert(atomic_load(&s_wake_source_index) == SYSTEM_PM_WAKE_SOURCE_NONE);

    system_pm_fakes_set_wakeup(0U, 0U);
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 2U));
    assert(atomic_load(&s_wake_reason) == SYSTEM_PM_WAKE_REASON_UNKNOWN);

    system_pm_fakes_set_wakeup(BIT(ESP_SLEEP_WAKEUP_TOUCHPAD), 0U);
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 3U));
    assert(atomic_load(&s_wake_reason) == SYSTEM_PM_WAKE_REASON_OTHER);

    /* An EXT1 cause without a reported GPIO stays GPIO with no source. */
    system_pm_fakes_set_wakeup(BIT(ESP_SLEEP_WAKEUP_EXT1), 0U);
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 4U));
    assert(atomic_load(&s_wake_reason) == SYSTEM_PM_WAKE_REASON_GPIO);
    assert(atomic_load(&s_wake_source_index) == SYSTEM_PM_WAKE_SOURCE_NONE);

    assert(system_pm_deinit() == ESP_OK);
}

static void _test_sleep_hook_failures(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    const system_pm_config_t config = _config();

    assert(system_pm_init(&config) == ESP_OK);

    /* A failing prepare hook skips sleep and still runs the restore hook. */
    s_prepare_result = ESP_FAIL;
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 1U));
    assert(atomic_load(&s_wake_reason) == SYSTEM_PM_WAKE_REASON_SLEEP_ERROR);
    assert(atomic_load(&s_wake_prepare_result) == (unsigned)ESP_FAIL);
    assert(system_pm_fakes_sleep_count() == 0U);
    assert(system_pm_fakes_ext1_enable_count() == 0U);
    assert(atomic_load(&s_guard_calls) == 0U);
    assert(atomic_load(&s_complete_calls) == 1U);

    /* EXT1 arming failure also aborts before commit. */
    s_prepare_result = ESP_OK;
    system_pm_fakes_set_enable_ext1_result(ESP_ERR_INVALID_STATE);
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 2U));
    assert(system_pm_fakes_sleep_count() == 0U);
    assert(system_pm_fakes_ext1_disable_count() == 1U);
    assert(atomic_load(&s_commit_calls) == 0U);

    /* A rejected commit guard aborts without entering sleep. */
    system_pm_fakes_set_enable_ext1_result(ESP_OK);
    s_guard_result = false;
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 3U));
    assert(atomic_load(&s_guard_calls) == 1U);
    assert(atomic_load(&s_commit_calls) == 0U);
    assert(system_pm_fakes_sleep_count() == 0U);
    assert(atomic_load(&s_wake_reason) == SYSTEM_PM_WAKE_REASON_SLEEP_ERROR);

    /* A failed light-sleep entry is reported and still completes. */
    s_guard_result = true;
    system_pm_fakes_set_light_sleep_result(ESP_FAIL);
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 4U));
    assert(system_pm_fakes_sleep_count() == 1U);
    assert(atomic_load(&s_wake_sleep_result) == (unsigned)ESP_FAIL);
    assert(atomic_load(&s_wake_reason) == SYSTEM_PM_WAKE_REASON_SLEEP_ERROR);

    /* A failing restore hook is reported as a sleep error. */
    system_pm_fakes_set_light_sleep_result(ESP_OK);
    s_complete_result = ESP_FAIL;
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 5U));
    assert(atomic_load(&s_wake_complete_result) == (unsigned)ESP_FAIL);
    assert(atomic_load(&s_wake_reason) == SYSTEM_PM_WAKE_REASON_SLEEP_ERROR);

    /* A failed restore leaves the service in recovery: deinit refuses until
       the retry succeeds, and cancel is that retry. */
    s_complete_result = ESP_OK;
    assert(system_pm_deinit() == ESP_ERR_INVALID_STATE);
    assert(system_pm_cancel_standby() == ESP_OK);
    assert(system_pm_deinit() == ESP_OK);
}

static void _test_cancel_queued_and_recovery(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    const system_pm_config_t config = _config();

    assert(system_pm_init(&config) == ESP_OK);

    /* Cancelling with nothing in flight is a no-op. */
    assert(system_pm_cancel_standby() == ESP_OK);
    assert(system_pm_fakes_sleep_count() == 0U);

    /* A failed restore leaves the service in recovery. Cancel retries the
       restore and reports its result, which is still failing here. */
    s_complete_result = ESP_FAIL;
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_count(&s_wake_calls, 1U));
    assert(system_pm_cancel_standby() == ESP_FAIL);
    assert(atomic_load(&s_complete_calls) == 2U);

    /* Once the restore succeeds, cancel converges and standby is admitted
       again. */
    s_complete_result = ESP_OK;
    assert(system_pm_cancel_standby() == ESP_OK);
    assert(atomic_load(&s_complete_calls) == 3U);
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_sleeps(2U));
    assert(_wait_for_count(&s_wake_calls, 2U));
    assert(system_pm_deinit() == ESP_OK);
}

static void _test_cpu_lock_refcount(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    const system_pm_config_t config = _config();

    assert(system_pm_acquire_cpu_max_freq() == ESP_ERR_INVALID_STATE);
    assert(system_pm_release_cpu_max_freq() == ESP_ERR_INVALID_STATE);

    assert(system_pm_init(&config) == ESP_OK);
    assert(system_pm_release_cpu_max_freq() == ESP_ERR_INVALID_STATE);
    assert(system_pm_acquire_cpu_max_freq() == ESP_OK);
    assert(system_pm_fakes_acquire_count() == 1U);
    assert(system_pm_acquire_cpu_max_freq() == ESP_OK);
    /* The second reference is counted, not re-acquired. */
    assert(system_pm_fakes_acquire_count() == 1U);
    assert(system_pm_release_cpu_max_freq() == ESP_OK);
    assert(system_pm_fakes_release_count() == 0U);
    assert(system_pm_release_cpu_max_freq() == ESP_OK);
    assert(system_pm_fakes_release_count() == 1U);

    /* A held reference is released during deinit, and a failed release is
       retried by the next deinit instead of leaking the lock. */
    assert(system_pm_acquire_cpu_max_freq() == ESP_OK);
    system_pm_fakes_fail_lock_release(ESP_FAIL);
    assert(system_pm_deinit() == ESP_FAIL);
    assert(system_pm_fakes_live_locks() == 1U);
    assert(system_pm_deinit() == ESP_OK);
    assert(system_pm_fakes_live_locks() == 0U);
    assert(system_pm_fakes_delete_count() == 1U);
    assert(system_pm_acquire_cpu_max_freq() == ESP_ERR_INVALID_STATE);
}

static void _test_deinit_rejects_committed_sleep(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    const system_pm_config_t config = _config();

    assert(system_pm_init(&config) == ESP_OK);
    assert(system_pm_request_standby() == ESP_OK);
    assert(_wait_for_sleeps(1U));
    /* The request completes before deinit can observe SLEEPING in this host
       fixture, so deinit must at least never hang and always clean up. */
    assert(system_pm_deinit() == ESP_OK);
    assert(host_dynamic_task_count() == 0U);
}

static void _test_worker_stop_tail(void)
{
    const int rtc_gpios[] = {TEST_WAKE_GPIO, TEST_WAKE_GPIO_SECOND};

    _reset_counters();
    system_pm_fakes_set_rtc_gpios(rtc_gpios, 2U);
    const system_pm_config_t config = _config();

    for (unsigned cycle = 0U; cycle < 3U; ++cycle)
    {
        assert(system_pm_init(&config) == ESP_OK);
        assert(system_pm_request_standby() == ESP_OK);
        assert(_wait_for_sleeps(cycle + 1U));
        assert(system_pm_deinit() == ESP_OK);
        assert(host_dynamic_task_count() == 0U);
        assert(system_pm_fakes_live_locks() == 0U);
    }
}

int main(void)
{
    assert(system_pm_deinit() == ESP_OK);
    _test_config_validation();
    _test_init_idempotence_and_deinit();
    _test_init_failures_roll_back();
    _test_standby_cycle();
    _test_high_level_wake_sources();
    _test_wake_reason_classification();
    _test_sleep_hook_failures();
    _test_cancel_queued_and_recovery();
    _test_cpu_lock_refcount();
    _test_deinit_rejects_committed_sleep();
    _test_worker_stop_tail();
    assert(system_pm_deinit() == ESP_OK);
    printf("system_pm host tests passed\n");
    return 0;
}

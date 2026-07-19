#include "host_freertos.h"
#include "power_service.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEST_WAIT_STEP_MS 1U
#define TEST_WAIT_LIMIT_MS 1000U
#define TEST_IRQ_STATUS UINT32_C(0x000a55a5)

static atomic_uint s_sample_calls;
static atomic_uint s_irq_poll_calls;
static atomic_uint s_irq_event_count;
static atomic_uint s_irq_event_attempts;
static atomic_uint s_irq_event_flags;
static atomic_uint s_irq_publish_failures;
static power_service_irq_event_t s_irq_event;

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * INT64_C(1000000) +
           (int64_t)now.tv_nsec / INT64_C(1000);
}

static void _sleep_ms(uint32_t milliseconds)
{
    struct timespec delay =
    {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static bool _wait_for_count(atomic_uint *counter, unsigned expected)
{
    for (uint32_t waited = 0U; waited < TEST_WAIT_LIMIT_MS;
            waited += TEST_WAIT_STEP_MS)
    {
        if (atomic_load_explicit(counter, memory_order_acquire) >= expected)
        {
            return true;
        }
        _sleep_ms(TEST_WAIT_STEP_MS);
    }
    return false;
}

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    assert(msg_id == POWER_SERVICE_MSG);
    if (sub_type == POWER_SERVICE_MSG_SUB_TYPE_IRQ)
    {
        atomic_fetch_add_explicit(&s_irq_event_attempts, 1U,
                                  memory_order_release);
        unsigned failures = atomic_load_explicit(&s_irq_publish_failures,
                            memory_order_acquire);
        if (failures != 0U && atomic_compare_exchange_strong_explicit(
                    &s_irq_publish_failures, &failures, failures - 1U,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return ESP_FAIL;
        }
        assert(payload != NULL);
        assert(payload_size == sizeof(s_irq_event));
        memcpy(&s_irq_event, payload, sizeof(s_irq_event));
        atomic_store_explicit(&s_irq_event_flags, flags, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_irq_event_count, 1U,
                                  memory_order_release);
    }
    return ESP_OK;
}

static bool _power_available(void)
{
    return true;
}

static esp_err_t _power_get_info(power_info_t *info)
{
    assert(info != NULL);
    *info = (power_info_t)
    {
        .battery_voltage_mv = 4012U,
        .battery_percent = 73,
        .is_charging = true,
        .is_vbus_connected = true,
    };
    atomic_fetch_add_explicit(&s_sample_calls, 1U, memory_order_release);
    return ESP_OK;
}

static esp_err_t _power_poll_irq(uint32_t *status)
{
    assert(status != NULL);
    const unsigned call = atomic_fetch_add_explicit(
                              &s_irq_poll_calls, 1U, memory_order_acq_rel) + 1U;
    *status = 0U;
    if (call == 2U)
    {
        return ESP_FAIL;
    }
    if (call == 3U)
    {
        *status = TEST_IRQ_STATUS;
    }
    return ESP_OK;
}

int main(void)
{
    const power_service_power_ops_t ops =
    {
        .is_available = _power_available,
        .get_info = _power_get_info,
        .poll_irq = _power_poll_irq,
    };
    atomic_store_explicit(&s_irq_publish_failures, 1U, memory_order_release);
    assert(power_service_register_power_ops(&ops) == ESP_OK);
    assert(power_service_init() == ESP_OK);

    assert(_wait_for_count(&s_irq_event_count, 1U));
    assert(atomic_load_explicit(&s_irq_poll_calls, memory_order_acquire) >= 3U);
    assert(atomic_load_explicit(&s_sample_calls, memory_order_acquire) == 1U);
    assert(atomic_load_explicit(&s_irq_event_flags, memory_order_relaxed) == 0U);
    assert(atomic_load_explicit(&s_irq_event_attempts, memory_order_acquire) ==
           2U);
    assert(s_irq_event.status == TEST_IRQ_STATUS);
    assert(s_irq_event.observed_at_ms > 0);

    assert(_wait_for_count(&s_sample_calls, 2U));
    assert(atomic_load_explicit(&s_irq_poll_calls, memory_order_acquire) > 3U);

    assert(power_service_suspend(500U) == ESP_OK);
    const unsigned paused_samples = atomic_load_explicit(
                                        &s_sample_calls, memory_order_acquire);
    const unsigned paused_polls = atomic_load_explicit(
                                      &s_irq_poll_calls, memory_order_acquire);
    _sleep_ms(30U);
    assert(atomic_load_explicit(&s_sample_calls, memory_order_acquire) ==
           paused_samples);
    assert(atomic_load_explicit(&s_irq_poll_calls, memory_order_acquire) ==
           paused_polls);

    assert(power_service_resume(500U) == ESP_OK);
    assert(_wait_for_count(&s_irq_poll_calls, paused_polls + 1U));
    assert(power_service_deinit() == ESP_OK);
    assert(host_freertos_wait_for_tasks(1000U));
    puts("power service IRQ regression passed");
    return 0;
}

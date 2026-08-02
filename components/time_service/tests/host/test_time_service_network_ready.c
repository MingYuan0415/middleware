#include "host_freertos.h"
#include "host_time_port.h"
#include "test_time_config.h"
#include "time_service.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define TEST_WAIT_ATTEMPTS 1000U

static void _sleep_one_ms(void)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static bool _wait_for_port_state(bool running, unsigned starts,
                                 unsigned stops)
{
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        if (host_time_port_is_running() == running &&
                host_time_port_start_count() == starts &&
                host_time_port_stop_count() == stops)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static void _complete_sync(int64_t epoch)
{
    assert(host_time_port_complete(epoch));
    assert(time_service_wait_sync(1000U) == ESP_OK);
}

int main(void)
{
    host_time_port_reset();
    assert(time_service_init(test_time_config()) == ESP_OK);
    assert(time_service_request_sync() == ESP_ERR_INVALID_STATE);

    assert(time_service_set_network_ready(true) == ESP_OK);
    assert(time_service_set_network_ready(true) == ESP_OK);
    assert(_wait_for_port_state(true, 1U, 0U));
    assert(time_service_set_network_ready(false) == ESP_OK);
    assert(_wait_for_port_state(false, 1U, 1U));
    assert(time_service_wait_sync(0U) == ESP_ERR_INVALID_STATE);
    assert(time_service_set_network_ready(false) == ESP_OK);
    assert(_wait_for_port_state(false, 1U, 1U));

    assert(time_service_set_network_ready(true) == ESP_OK);
    assert(_wait_for_port_state(true, 2U, 1U));
    _complete_sync(INT64_C(1704067200));
    assert(_wait_for_port_state(false, 2U, 2U));
    assert(time_service_set_network_ready(true) == ESP_OK);
    assert(_wait_for_port_state(false, 2U, 2U));
    assert(host_time_port_restart_count() == 0U);

    assert(time_service_request_sync() == ESP_OK);
    assert(_wait_for_port_state(true, 3U, 2U));
    assert(host_time_port_restart_count() == 0U);
    _complete_sync(INT64_C(1704067260));
    assert(_wait_for_port_state(false, 3U, 3U));

    assert(time_service_set_network_ready(false) == ESP_OK);
    assert(time_service_set_network_ready(false) == ESP_OK);
    assert(_wait_for_port_state(false, 3U, 3U));
    assert(time_service_request_sync() == ESP_ERR_INVALID_STATE);
    assert(host_time_port_restart_count() == 0U);

    assert(time_service_set_network_ready(true) == ESP_OK);
    assert(_wait_for_port_state(true, 4U, 3U));
    _complete_sync(INT64_C(1704067320));
    assert(_wait_for_port_state(false, 4U, 4U));

    assert(time_service_suspend(1000U) == ESP_OK);
    assert(_wait_for_port_state(false, 4U, 4U));
    assert(time_service_resume(1000U) == ESP_OK);
    assert(_wait_for_port_state(false, 4U, 4U));
    assert(time_service_set_network_ready(true) == ESP_OK);
    assert(_wait_for_port_state(true, 5U, 4U));
    _complete_sync(INT64_C(1704067380));
    assert(_wait_for_port_state(false, 5U, 5U));

    assert(time_service_deinit() == ESP_OK);
    assert(host_freertos_wait_for_tasks(1000U));
    assert(!host_time_port_is_running());
    assert(host_time_port_stop_count() == 5U);
    puts("time_service network-ready regression passed");
    return 0;
}

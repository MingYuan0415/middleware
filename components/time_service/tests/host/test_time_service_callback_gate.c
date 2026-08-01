#include "host_freertos.h"
#include "host_time_port.h"
#include "time_service.h"
#include "test_time_config.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void _test_init_config_boundaries(void)
{
    time_service_config_t config = *test_time_config();
    config.timezone = "";
    assert(time_service_init(&config) == ESP_ERR_INVALID_ARG);
    config = *test_time_config();
    config.sntp_server = "";
    assert(time_service_init(&config) == ESP_ERR_INVALID_ARG);

    char timezone[TIME_SERVICE_TIMEZONE_MAX_BYTES + 2U];
    char server[TIME_SERVICE_SNTP_SERVER_MAX_BYTES + 2U];
    memset(timezone, 'T', sizeof(timezone));
    memset(server, 'n', sizeof(server));
    timezone[TIME_SERVICE_TIMEZONE_MAX_BYTES] = '\0';
    server[TIME_SERVICE_SNTP_SERVER_MAX_BYTES] = '\0';
    config = (time_service_config_t)
    {
        .timezone = timezone,
        .sntp_server = server,
        .task_priority = 4U,
    };
    assert(time_service_init(&config) == ESP_OK);
    assert(time_service_deinit() == ESP_OK);
    assert(host_freertos_wait_for_tasks(1000U));

    timezone[TIME_SERVICE_TIMEZONE_MAX_BYTES] = 'T';
    timezone[TIME_SERVICE_TIMEZONE_MAX_BYTES + 1U] = '\0';
    assert(time_service_init(&config) == ESP_ERR_INVALID_ARG);
    timezone[TIME_SERVICE_TIMEZONE_MAX_BYTES] = '\0';
    server[TIME_SERVICE_SNTP_SERVER_MAX_BYTES] = 'n';
    server[TIME_SERVICE_SNTP_SERVER_MAX_BYTES + 1U] = '\0';
    assert(time_service_init(&config) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    host_time_port_reset();
    assert(time_service_init(NULL) == ESP_ERR_INVALID_ARG);
    _test_init_config_boundaries();
    host_time_port_reset();

    char timezone[] = "CST-8";
    char server[] = "pool.ntp.org";
    const time_service_config_t mutable_config =
    {
        .timezone = timezone,
        .sntp_server = server,
        .task_priority = 4U,
    };
    assert(time_service_init(&mutable_config) == ESP_OK);
    timezone[0] = 'X';
    server[0] = 'X';
    assert(time_service_init(test_time_config()) == ESP_OK);
    time_service_config_t different = *test_time_config();
    different.task_priority++;
    assert(time_service_init(&different) == ESP_ERR_INVALID_STATE);
    const uint32_t initial_notification_count =
        host_freertos_notification_count();
    assert(time_service_request_sync() == ESP_OK);
    assert(host_freertos_notification_count() == initial_notification_count);

    host_time_port_invoke_callback_on_stop(true);
    assert(time_service_cancel_sync() == ESP_OK);
    assert(host_freertos_notification_count() == initial_notification_count);

    assert(time_service_request_sync() == ESP_OK);
    host_time_port_set_stop_result(ESP_FAIL);
    assert(time_service_cancel_sync() == ESP_FAIL);
    assert(time_service_wait_sync(0U) == ESP_FAIL);
    host_time_port_set_stop_result(ESP_OK);
    assert(time_service_cancel_sync() == ESP_OK);

    host_time_port_invoke_callback_on_stop(false);
    assert(time_service_request_sync() == ESP_OK);
    assert(host_time_port_complete(INT64_C(1704067200)));
    assert(time_service_wait_sync(1000U) == ESP_OK);
    assert(host_freertos_notification_count() ==
           initial_notification_count + 1U);

    host_time_port_set_stop_result(ESP_FAIL);
    assert(time_service_deinit() == ESP_FAIL);
    host_time_port_set_stop_result(ESP_OK);
    assert(time_service_deinit() == ESP_OK);
    assert(host_freertos_wait_for_tasks(1000U));
    puts("time_service callback gate regression passed");
    return 0;
}

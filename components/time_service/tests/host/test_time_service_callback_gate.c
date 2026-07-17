#include "host_freertos.h"
#include "host_time_port.h"
#include "time_service.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    host_time_port_reset();
    assert(time_service_init() == ESP_OK);
    assert(time_service_request_sync() == ESP_OK);
    assert(host_freertos_notification_count() == 0U);

    host_time_port_invoke_callback_on_stop(true);
    assert(time_service_cancel_sync() == ESP_OK);
    assert(host_freertos_notification_count() == 0U);

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
    assert(host_freertos_notification_count() == 1U);

    host_time_port_set_stop_result(ESP_FAIL);
    assert(time_service_deinit() == ESP_FAIL);
    host_time_port_set_stop_result(ESP_OK);
    assert(time_service_deinit() == ESP_OK);
    assert(host_freertos_wait_for_tasks(1000U));
    puts("time_service callback gate regression passed");
    return 0;
}

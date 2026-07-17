#include "host_time_idf.h"
#include "time_service_port.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    host_time_idf_reset();
    assert(time_service_port_sntp_stop() == ESP_OK);

    assert(host_time_idf_call_count() == 3U);
    assert(host_time_idf_call_at(0) == HOST_TIME_IDF_CALLBACK_DETACHED);
    assert(host_time_idf_call_at(1) == HOST_TIME_IDF_SNTP_STOPPED);
    assert(host_time_idf_call_at(2) == HOST_TIME_IDF_TCPIP_BARRIER);

    host_time_idf_reset();
    host_time_idf_set_barrier_result(ESP_FAIL);
    assert(time_service_port_sntp_stop() == ESP_FAIL);
    assert(host_time_idf_call_count() == 3U);
    assert(host_time_idf_call_at(0) == HOST_TIME_IDF_CALLBACK_DETACHED);
    assert(host_time_idf_call_at(1) == HOST_TIME_IDF_SNTP_STOPPED);
    assert(host_time_idf_call_at(2) == HOST_TIME_IDF_TCPIP_BARRIER);
    puts("time_service TCP/IP barrier regression passed");
    return 0;
}

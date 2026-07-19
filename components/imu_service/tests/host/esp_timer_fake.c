#include "esp_timer.h"
#include "host_timer.h"

#include <stdatomic.h>
#include <stdint.h>

static atomic_int_fast64_t s_time_us = ATOMIC_VAR_INIT(INT64_C(1000000));

int64_t esp_timer_get_time(void)
{
    return atomic_fetch_add_explicit(&s_time_us, INT64_C(1000),
                                     memory_order_relaxed);
}

void host_timer_reset(void)
{
    atomic_store_explicit(&s_time_us, INT64_C(1000000), memory_order_relaxed);
}

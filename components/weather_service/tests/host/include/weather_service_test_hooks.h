#ifndef __WEATHER_SERVICE_TEST_HOOKS_H__
#define __WEATHER_SERVICE_TEST_HOOKS_H__

#include <stdbool.h>
#include <stdint.h>

typedef struct weather_service_test_cycle_state
{
    uint64_t lifecycle_epoch;
    uint64_t pending_scope_session;
    bool pending_scope_attempted;
    uint8_t drift_attempt;
    int64_t retry_due_ms;
    bool cache_dirty;
    int64_t last_cache_write_ms;
} weather_service_test_cycle_state_t;

bool weather_service_test_get_cycle_state(
    weather_service_test_cycle_state_t *state);
bool weather_service_test_lock_state(void);
void weather_service_test_unlock_state(void);

#endif /* __WEATHER_SERVICE_TEST_HOOKS_H__ */

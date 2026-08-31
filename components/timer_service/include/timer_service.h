#ifndef __TIMER_SERVICE_H__
#define __TIMER_SERVICE_H__

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    TIMER_SERVICE_IDLE = 0,
    TIMER_SERVICE_RUNNING,
    TIMER_SERVICE_PAUSED,
    TIMER_SERVICE_COMPLETED,
} timer_service_state_t;

typedef enum
{
    TIMER_SERVICE_FOCUS_WORK = 0,
    TIMER_SERVICE_FOCUS_BREAK,
} timer_service_focus_phase_t;

typedef struct timer_service_config
{
    int64_t (*monotonic_time_us)(void);
} timer_service_config_t;

typedef struct timer_service_snapshot
{
    uint32_t generation;
    timer_service_state_t countdown_state;
    uint32_t countdown_duration_ms;
    uint32_t countdown_remaining_ms;
    timer_service_state_t stopwatch_state;
    uint64_t stopwatch_elapsed_ms;
    timer_service_state_t focus_state;
    timer_service_focus_phase_t focus_phase;
    uint32_t focus_remaining_ms;
    uint32_t focus_completed_cycles;
} timer_service_snapshot_t;

esp_err_t timer_service_init(const timer_service_config_t *config);
esp_err_t timer_service_deinit(void);
esp_err_t timer_service_get_snapshot(timer_service_snapshot_t *snapshot);

esp_err_t timer_service_countdown_start(uint32_t duration_ms);
esp_err_t timer_service_countdown_pause(void);
esp_err_t timer_service_countdown_resume(void);
esp_err_t timer_service_countdown_reset(void);

esp_err_t timer_service_stopwatch_start(void);
esp_err_t timer_service_stopwatch_pause(void);
esp_err_t timer_service_stopwatch_reset(void);

esp_err_t timer_service_focus_start(uint32_t work_ms, uint32_t break_ms);
esp_err_t timer_service_focus_pause(void);
esp_err_t timer_service_focus_resume(void);
esp_err_t timer_service_focus_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* __TIMER_SERVICE_H__ */

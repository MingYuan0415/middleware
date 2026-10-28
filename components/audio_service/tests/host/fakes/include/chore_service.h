#ifndef __CHORE_SERVICE_H__
#define __CHORE_SERVICE_H__

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct chore_service_cancel_token
{
    const atomic_bool *requested;
} chore_service_cancel_token_t;

typedef struct chore_service_job
{
    void (*run)(const chore_service_cancel_token_t *cancel, void *arg);
    void (*release)(void *arg);
    void *arg;
    uint32_t delay_ms;
    uint32_t period_ms;
} chore_service_job_t;

typedef struct chore_service_handle
{
    uint32_t slot;
    uint32_t generation;
} chore_service_handle_t;

esp_err_t chore_service_submit(const chore_service_job_t *job,
                               chore_service_handle_t *handle);

#endif /* __CHORE_SERVICE_H__ */

#ifndef __ONBOARDING_SERVICE_H__
#define __ONBOARDING_SERVICE_H__

#include "esp_err.h"

typedef enum
{
    ONBOARDING_SERVICE_PENDING = 0,
    ONBOARDING_SERVICE_DEFERRED = 1,
    ONBOARDING_SERVICE_COMPLETED = 2,
} onboarding_service_state_t;

esp_err_t onboarding_service_init(void);
esp_err_t onboarding_service_deinit(void);
esp_err_t onboarding_service_get_state(onboarding_service_state_t *state);
esp_err_t onboarding_service_defer(void);
esp_err_t onboarding_service_complete(void);
esp_err_t onboarding_service_reset(void);

#endif /* __ONBOARDING_SERVICE_H__ */

#include "onboarding_service.h"

#include "nv_storage.h"
#include "nvs.h"

#include <stdbool.h>
#include <stdint.h>

#define ONBOARDING_STORAGE_KEY "onboard_state"

static bool s_initialized;
static onboarding_service_state_t s_state;

static esp_err_t _onboarding_store(onboarding_service_state_t state)
{
    esp_err_t result = nv_storage_set_u8(ONBOARDING_STORAGE_KEY,
                                         (uint8_t)state);
    if (result == ESP_OK)
    {
        s_state = state;
    }
    return result;
}

esp_err_t onboarding_service_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t stored = 0U;
    esp_err_t result = nv_storage_get_u8(ONBOARDING_STORAGE_KEY, &stored);
    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        result = nv_storage_set_u8(ONBOARDING_STORAGE_KEY,
                                   ONBOARDING_SERVICE_PENDING);
        stored = ONBOARDING_SERVICE_PENDING;
    }
    if (result != ESP_OK)
    {
        return result;
    }
    if (stored > ONBOARDING_SERVICE_COMPLETED)
    {
        /* A torn or manually edited byte must not brick the first-run path. */
        result = nv_storage_set_u8(ONBOARDING_STORAGE_KEY,
                                   ONBOARDING_SERVICE_PENDING);
        if (result != ESP_OK)
        {
            return result;
        }
        stored = ONBOARDING_SERVICE_PENDING;
    }
    s_state = (onboarding_service_state_t)stored;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t onboarding_service_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_initialized = false;
    s_state = ONBOARDING_SERVICE_PENDING;
    return ESP_OK;
}

esp_err_t onboarding_service_get_state(onboarding_service_state_t *state)
{
    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *state = s_state;
    return ESP_OK;
}

esp_err_t onboarding_service_defer(void)
{
    return s_initialized ? _onboarding_store(ONBOARDING_SERVICE_DEFERRED) :
           ESP_ERR_INVALID_STATE;
}

esp_err_t onboarding_service_complete(void)
{
    return s_initialized ? _onboarding_store(ONBOARDING_SERVICE_COMPLETED) :
           ESP_ERR_INVALID_STATE;
}

esp_err_t onboarding_service_reset(void)
{
    return s_initialized ? _onboarding_store(ONBOARDING_SERVICE_PENDING) :
           ESP_ERR_INVALID_STATE;
}

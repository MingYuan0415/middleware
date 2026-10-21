#ifndef __SYSTEM_PM_HOST_ESP_PM_H__
#define __SYSTEM_PM_HOST_ESP_PM_H__

#include "esp_err.h"

typedef enum
{
    ESP_PM_CPU_FREQ_MAX = 0,
} esp_pm_lock_type_t;

typedef struct esp_pm_lock *esp_pm_lock_handle_t;

esp_err_t esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg,
                             const char *name,
                             esp_pm_lock_handle_t *out_handle);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle);
esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t handle);

#endif /* __SYSTEM_PM_HOST_ESP_PM_H__ */

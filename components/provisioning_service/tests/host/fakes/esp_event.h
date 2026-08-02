#ifndef __PROVISIONING_HOST_ESP_EVENT_H__
#define __PROVISIONING_HOST_ESP_EVENT_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *argument,
                                    esp_event_base_t event_base,
                                    int32_t event_id, void *event_data);

#define ESP_EVENT_ANY_ID (-1)
#define ESP_EVENT_DECLARE_BASE(name) extern esp_event_base_t name

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_t handler, void *argument,
    esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_instance_t instance);

#endif /* __PROVISIONING_HOST_ESP_EVENT_H__ */

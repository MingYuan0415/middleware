#ifndef __WEATHER_HOST_EVENT_BUS_H__
#define __WEATHER_HOST_EVENT_BUS_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef const void *event_bus_msg_id_t;

#define EVENT_BUS_DECLARE_ID(name) extern const event_bus_msg_id_t name
#define EVENT_BUS_DEFINE_ID(name)       \
    static const uint8_t s_##name##_id; \
    const event_bus_msg_id_t name = &s_##name##_id
#define EVENT_BUS_PUBLISH_FLAG_UI_LATEST (UINT32_C(1) << 1)

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags);

#endif /* __WEATHER_HOST_EVENT_BUS_H__ */

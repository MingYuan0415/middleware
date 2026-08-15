#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event_bus.h"

EVENT_BUS_DEFINE_ID(CONNECTIVITY_MANAGER_MSG);

static event_bus_cb_t s_callback = NULL;
static void *s_callback_arg = NULL;
static uint32_t s_sub_type = EVENT_BUS_SUB_TYPE_ANY;
static event_bus_sub_handle_t s_next_handle = 1U;

esp_err_t event_bus_init(void)
{
    return ESP_OK;
}

esp_err_t event_bus_subscribe(event_bus_msg_id_t msg_id, uint32_t sub_type,
                              event_bus_cb_t cb, void *user_data,
                              event_bus_dispatch_context_t context,
                              event_bus_sub_handle_t *out_handle)
{
    if (msg_id == NULL || cb == NULL || out_handle == NULL ||
            context != EVENT_BUS_DISPATCH_PUBLISHER)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_callback != NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_callback = cb;
    s_callback_arg = user_data;
    s_sub_type = sub_type;
    *out_handle = s_next_handle++;
    return ESP_OK;
}

esp_err_t event_bus_unsubscribe(event_bus_sub_handle_t handle)
{
    if (handle == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_callback == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    s_callback = NULL;
    s_callback_arg = NULL;
    return ESP_OK;
}

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    (void)flags;
    if (s_callback != NULL &&
            (s_sub_type == EVENT_BUS_SUB_TYPE_ANY || s_sub_type == sub_type))
    {
        s_callback(msg_id, sub_type, payload, payload_size, s_callback_arg);
    }
    return ESP_OK;
}

/* Test hook: publish one message through the single-slot bus. */
void event_bus_fake_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size)
{
    (void)event_bus_publish(msg_id, sub_type, payload, payload_size, 0U);
}

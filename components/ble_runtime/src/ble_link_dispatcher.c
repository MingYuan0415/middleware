#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_dispatcher.h"

#define DBG_TAG "ble_link_dispatcher"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

#define BLE_LINK_DISPATCHER_REQUEST_TAGS 5U
#define BLE_LINK_DISPATCHER_SESSION_IDS_INITIAL 16U

typedef struct ble_link_dispatcher_handler
{
    ble_link_codec_request_tag_t tag;
    ble_link_request_handler_t handler;
    void *arg;
    bool registered;
} ble_link_dispatcher_handler_t;

typedef struct ble_link_dispatcher
{
    ble_link_dispatcher_handler_t handlers[BLE_LINK_DISPATCHER_REQUEST_TAGS];
    uint64_t *session_ids;
    size_t session_id_count;
    size_t session_id_capacity;
} ble_link_dispatcher_t;

static ble_link_dispatcher_t s_dispatcher;

static ble_link_dispatcher_handler_t *_ble_link_dispatcher_find_handler(
    ble_link_codec_request_tag_t tag)
{
    for (size_t i = 0U; i < BLE_LINK_DISPATCHER_REQUEST_TAGS; ++i)
    {
        if (s_dispatcher.handlers[i].registered &&
                s_dispatcher.handlers[i].tag == tag)
        {
            return &s_dispatcher.handlers[i];
        }
    }
    return NULL;
}

static bool _ble_link_dispatcher_session_id_exists(uint64_t request_id)
{
    for (size_t i = 0U; i < s_dispatcher.session_id_count; ++i)
    {
        if (s_dispatcher.session_ids[i] == request_id)
        {
            return true;
        }
    }
    return false;
}

static bool _ble_link_dispatcher_add_session_id(uint64_t request_id)
{
    if (s_dispatcher.session_ids == NULL)
    {
        s_dispatcher.session_ids = malloc(
                                       BLE_LINK_DISPATCHER_SESSION_IDS_INITIAL *
                                       sizeof(s_dispatcher.session_ids[0]));
        if (s_dispatcher.session_ids == NULL)
        {
            return false;
        }
        s_dispatcher.session_id_capacity =
            BLE_LINK_DISPATCHER_SESSION_IDS_INITIAL;
    }
    if (s_dispatcher.session_id_count >= s_dispatcher.session_id_capacity)
    {
        const size_t new_capacity = s_dispatcher.session_id_capacity * 2U;
        uint64_t *grown = realloc(
                              s_dispatcher.session_ids,
                              new_capacity * sizeof(s_dispatcher.session_ids[0]));

        if (grown == NULL)
        {
            return false;
        }
        s_dispatcher.session_ids = grown;
        s_dispatcher.session_id_capacity = new_capacity;
    }
    s_dispatcher.session_ids[s_dispatcher.session_id_count] = request_id;
    s_dispatcher.session_id_count++;
    return true;
}

esp_err_t ble_link_dispatcher_handle_request(
    const ble_link_codec_envelope_t *envelope,
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, uint32_t *out_error)
{
    ble_link_dispatcher_handler_t *handler;

    if (envelope == NULL || request == NULL || facts == NULL || out_error == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_error = 0U;
    /* Boot id validation precedes the protocol check: a foreign boot is
     * terminal per the lifecycle contract. */
    if (facts->active_boot_id == 0U ||
            envelope->boot_id != facts->active_boot_id)
    {
        *out_error = BLE_LINK_ERROR_UNAVAILABLE;
        return ESP_OK;
    }
    if (envelope->protocol_major != BLE_LINK_CODEC_PROTOCOL_MAJOR)
    {
        *out_error = BLE_LINK_ERROR_UNSUPPORTED_VERSION;
        return ESP_OK;
    }
    for (size_t i = 0U; i < envelope->flags_count; ++i)
    {
        if (envelope->flags_values[i] != BLE_LINK_CODEC_FLAG_RECOVERY_QUERY)
        {
            *out_error = BLE_LINK_ERROR_INVALID_ARGUMENT;
            return ESP_OK;
        }
    }
    if (request->request_id == 0U)
    {
        *out_error = BLE_LINK_ERROR_INVALID_ARGUMENT;
        return ESP_OK;
    }
    if (_ble_link_dispatcher_session_id_exists(request->request_id))
    {
        *out_error = BLE_LINK_ERROR_CONFLICT;
        return ESP_OK;
    }
    if (!_ble_link_dispatcher_add_session_id(request->request_id))
    {
        *out_error = BLE_LINK_ERROR_RESOURCE_EXHAUSTED;
        return ESP_ERR_NO_MEM;
    }
    handler = _ble_link_dispatcher_find_handler(request->body);
    if (handler == NULL)
    {
        *out_error = BLE_LINK_ERROR_UNSUPPORTED_OPERATION;
        return ESP_OK;
    }
    *out_error = handler->handler(request, facts, handler->arg);
    return ESP_OK;
}

esp_err_t ble_link_dispatcher_register_request(
    ble_link_codec_request_tag_t tag,
    ble_link_request_handler_t handler, void *arg)
{
    if (handler == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    switch (tag)
    {
    case BLE_LINK_CODEC_REQUEST_GET_CAPABILITIES:
    case BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT:
    case BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE:
    case BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT:
    case BLE_LINK_CODEC_REQUEST_SUBSCRIBE_EVENTS:
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (_ble_link_dispatcher_find_handler(tag) != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0U; i < BLE_LINK_DISPATCHER_REQUEST_TAGS; ++i)
    {
        if (!s_dispatcher.handlers[i].registered)
        {
            s_dispatcher.handlers[i].tag = tag;
            s_dispatcher.handlers[i].handler = handler;
            s_dispatcher.handlers[i].arg = arg;
            s_dispatcher.handlers[i].registered = true;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

void ble_link_dispatcher_clear_session(void)
{
    free(s_dispatcher.session_ids);
    s_dispatcher.session_ids = NULL;
    s_dispatcher.session_id_count = 0U;
    s_dispatcher.session_id_capacity = 0U;
}

void ble_link_dispatcher_reset(void)
{
    free(s_dispatcher.session_ids);
    memset(&s_dispatcher, 0, sizeof(s_dispatcher));
}

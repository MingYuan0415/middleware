#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_link_operation.h"
#include "device_link_router.h"
#include "device_link_wire.h"

typedef struct handler_state
{
    unsigned calls;
    uint8_t value;
} handler_state_t;

static const device_link_tlv_field_rule_t s_value_rule =
{
    .id = 1U,
    .wire_type = DEVICE_LINK_TLV_UNSIGNED,
    .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
    .maximum_unsigned = UINT64_MAX,
};

static const device_link_tlv_schema_t s_value_schema =
{
    .fields = &s_value_rule,
    .field_count = 1U,
    .maximum_encoded_bytes = 3U,
};

static esp_err_t _digest(
    const uint8_t *request, size_t request_len,
    uint8_t digest[DEVICE_LINK_REPLAY_DIGEST_BYTES], void *arg)
{
    (void)arg;
    uint32_t value = 2166136261U;
    for (size_t i = 0U; i < request_len; ++i)
    {
        value ^= request[i];
        value *= 16777619U;
    }
    memset(digest, 0, DEVICE_LINK_REPLAY_DIGEST_BYTES);
    memcpy(digest, &value, sizeof(value));
    return ESP_OK;
}

static device_link_status_t _handler(
    const device_link_request_context_t *context,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    void *arg)
{
    handler_state_t *state = arg;

    assert(context->header.domain_id == DEVICE_LINK_DOMAIN_WIFI);
    state->calls++;
    if (request_len != 2U || request[0] != 0x04U ||
            request[1] != state->value || response_capacity < 2U)
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    response[0] = 0x04U;
    response[1] = (uint8_t)(request[1] + 1U);
    *response_len = 2U;
    return DEVICE_LINK_STATUS_OK;
}

static size_t _make_request(
    uint32_t call_id, uint64_t boot_id, uint8_t value,
    uint8_t *out, size_t capacity)
{
    assert(capacity >= DEVICE_LINK_WIRE_HEADER_BYTES + 2U);
    const device_link_wire_header_t header =
    {
        .kind = DEVICE_LINK_MESSAGE_REQUEST,
        .domain_id = DEVICE_LINK_DOMAIN_WIFI,
        .domain_major = 1U,
        .method_id = 4U,
        .call_id = call_id,
        .boot_id = boot_id,
    };
    assert(device_link_wire_encode_header(&header, out) == ESP_OK);
    out[DEVICE_LINK_WIRE_HEADER_BYTES] = 0x04U;
    out[DEVICE_LINK_WIRE_HEADER_BYTES + 1U] = value;
    return DEVICE_LINK_WIRE_HEADER_BYTES + 2U;
}

static device_link_status_t _response_status(
    const uint8_t *response, size_t response_len)
{
    device_link_status_t status = 0;

    assert(response_len >= DEVICE_LINK_WIRE_HEADER_BYTES + 2U);
    assert(device_link_wire_decode_status(
               &response[DEVICE_LINK_WIRE_HEADER_BYTES],
               response_len - DEVICE_LINK_WIRE_HEADER_BYTES,
               &status) == ESP_OK);
    return status;
}

static void _test_router(void)
{
    handler_state_t handler = {.value = 0x31U};
    const device_link_method_descriptor_t methods[] =
    {
        {
            .method_id = 4U,
            .channel = DEVICE_LINK_CHANNEL_CONTROL,
            .permission_id = DEVICE_LINK_PERMISSION_WIFI_WRITE,
            .maximum_request_bytes = 2U,
            .maximum_response_bytes = 2U,
            .request_schema = &s_value_schema,
            .response_schema = &s_value_schema,
            .response_body_status_mask = DEVICE_LINK_STATUS_MASK(
                                             DEVICE_LINK_STATUS_OK),
            .handler = _handler,
            .handler_arg = &handler,
        },
    };
    const device_link_domain_descriptor_t domains[] =
    {
        {
            .domain_id = DEVICE_LINK_DOMAIN_WIFI,
            .major = 1U,
            .methods = methods,
            .method_count = 1U,
        },
    };
    uint8_t replay_response[DEVICE_LINK_REPLAY_SLOTS][64];
    device_link_call_replay_t replay[DEVICE_LINK_REPLAY_SLOTS];
    memset(replay, 0, sizeof(replay));
    for (size_t i = 0U; i < DEVICE_LINK_REPLAY_SLOTS; ++i)
    {
        replay[i].response = replay_response[i];
        replay[i].response_capacity = sizeof(replay_response[i]);
    }
    device_link_router_t router;
    const uint64_t boot_id = UINT64_C(0x0102030405060708);
    const uint16_t permissions[] = {DEVICE_LINK_PERMISSION_WIFI_WRITE};
    device_link_request_context_t facts =
    {
        .channel = DEVICE_LINK_CHANNEL_CONTROL,
        .admission = DEVICE_LINK_ADMISSION_AUTHORIZED,
        .security_authenticated = true,
        .authorized = true,
        .permissions = permissions,
        .permission_count = 1U,
    };
    uint8_t request[64];
    uint8_t response[64];
    size_t response_len = 0U;
    size_t request_len = _make_request(
                             1U, boot_id, handler.value,
                             request, sizeof(request));

    device_link_router_t invalid_router;
    assert(device_link_router_init(
               &invalid_router, boot_id, domains, 1U, replay, 3U,
               _digest, NULL) == ESP_ERR_INVALID_ARG);

    assert(device_link_router_init(
               &router, boot_id, domains, 1U, replay,
               DEVICE_LINK_REPLAY_SLOTS,
               _digest, NULL) == ESP_OK);
    assert(device_link_router_is_sealed(&router));
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(_response_status(response, response_len) == DEVICE_LINK_STATUS_OK);
    assert(response[response_len - 1U] == handler.value + 1U);
    assert(handler.calls == 1U);

    uint8_t first_response[64];
    const size_t first_len = response_len;
    memcpy(first_response, response, first_len);
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(response_len == first_len);
    assert(memcmp(response, first_response, first_len) == 0);
    assert(handler.calls == 1U);

    request[DEVICE_LINK_WIRE_HEADER_BYTES + 1U]++;
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(_response_status(response, response_len) ==
           DEVICE_LINK_STATUS_CONFLICT);
    assert(handler.calls == 1U);

    request_len = _make_request(
                      2U, boot_id, handler.value,
                      request, sizeof(request));
    facts.permissions = NULL;
    facts.permission_count = 0U;
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(_response_status(response, response_len) ==
           DEVICE_LINK_STATUS_PERMISSION_DENIED);
    assert(handler.calls == 1U);

    request_len = _make_request(
                      4U, boot_id, handler.value,
                      request, sizeof(request));
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(_response_status(response, response_len) ==
           DEVICE_LINK_STATUS_CONFLICT);

    const uint16_t invalid_permissions[] =
    {
        DEVICE_LINK_PERMISSION_WIFI_WRITE,
        DEVICE_LINK_PERMISSION_WIFI_READ,
    };
    facts.permissions = invalid_permissions;
    facts.permission_count = 2U;
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) ==
           ESP_ERR_INVALID_ARG);
}

static esp_err_t _cancel_owner(uint64_t owner_id, void *arg)
{
    uint64_t *seen = arg;

    *seen = owner_id;
    return ESP_OK;
}

static void _test_operations(void)
{
    device_link_operation_table_t table;
    uint64_t ids[DEVICE_LINK_MAX_OPERATIONS];
    uint64_t canceled_owner = 0U;

    assert(device_link_operation_table_init(&table, 9U) == ESP_OK);
    for (size_t i = 0U; i < DEVICE_LINK_MAX_OPERATIONS; ++i)
    {
        assert(device_link_operation_start(
                   &table, 10U, DEVICE_LINK_DOMAIN_WIFI, 4U,
                   i + 100U, _cancel_owner, &canceled_owner,
                   &ids[i]) == ESP_OK);
        assert(ids[i] == i + 1U);
    }
    uint64_t extra = 0U;
    assert(device_link_operation_start(
               &table, 10U, DEVICE_LINK_DOMAIN_WIFI, 4U, 999U,
               NULL, NULL, &extra) == ESP_ERR_NO_MEM);
    assert(device_link_operation_update(
               &table, 20U, ids[0], DEVICE_LINK_OPERATION_RUNNING,
               DEVICE_LINK_STATUS_OK, NULL, 0U) == ESP_OK);
    assert(device_link_operation_cancel(
               &table, 30U, ids[0]) == ESP_OK);
    assert(canceled_owner == 100U);
    device_link_operation_t operation;
    assert(device_link_operation_get(
               &table, 30U, ids[0], &operation) == ESP_OK);
    assert(operation.state == DEVICE_LINK_OPERATION_CANCELED);
    assert(device_link_operation_get(
               &table, 30U + DEVICE_LINK_OPERATION_RETENTION_MS,
               ids[0], &operation) == ESP_ERR_NOT_FOUND);
    assert(device_link_operation_start(
               &table, 30U + DEVICE_LINK_OPERATION_RETENTION_MS,
               DEVICE_LINK_DOMAIN_WIFI, 4U, 1000U,
               NULL, NULL, &extra) == ESP_OK);
    assert(extra == 5U);
}

int main(void)
{
    _test_router();
    _test_operations();
    puts("device_link router/operation tests passed");
    return 0;
}

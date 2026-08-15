#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "device_link_router.h"
#include "device_link_wire.h"

static void _secure_zero(void *data, size_t length)
{
    volatile uint8_t *bytes = data;
    while (length-- != 0U)
    {
        *bytes++ = 0U;
    }
}

static const device_link_domain_descriptor_t *_find_domain(
    const device_link_router_t *router, uint8_t domain_id)
{
    for (size_t i = 0U; i < router->domain_count; ++i)
    {
        if (router->domains[i].domain_id == domain_id)
        {
            return &router->domains[i];
        }
    }
    return NULL;
}

static const device_link_method_descriptor_t *_find_method(
    const device_link_domain_descriptor_t *domain, uint8_t method_id)
{
    for (size_t i = 0U; i < domain->method_count; ++i)
    {
        if (domain->methods[i].method_id == method_id)
        {
            return &domain->methods[i];
        }
    }
    return NULL;
}

bool device_link_permission_set_valid(
    const uint16_t *permissions, size_t permission_count)
{
    if ((permissions == NULL && permission_count != 0U) ||
            permission_count > DEVICE_LINK_MAX_PERMISSIONS)
    {
        return false;
    }
    for (size_t i = 0U; i < permission_count; ++i)
    {
        if (permissions[i] == 0U ||
                (i > 0U && permissions[i - 1U] >= permissions[i]))
        {
            return false;
        }
    }
    return true;
}

bool device_link_permission_contains(
    const uint16_t *permissions, size_t permission_count,
    uint16_t permission)
{
    if (!device_link_permission_set_valid(permissions, permission_count) ||
            permission == 0U)
    {
        return false;
    }
    for (size_t i = 0U; i < permission_count; ++i)
    {
        if (permissions[i] == permission)
        {
            return true;
        }
        if (permissions[i] > permission)
        {
            break;
        }
    }
    return false;
}

esp_err_t device_link_domain_descriptors_validate(
    const device_link_domain_descriptor_t *domains, size_t domain_count)
{
    if (domains == NULL || domain_count == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < domain_count; ++i)
    {
        const device_link_domain_descriptor_t *domain = &domains[i];

        if (domain->domain_id == DEVICE_LINK_DOMAIN_INVALID ||
                domain->major == 0U || domain->methods == NULL ||
                domain->method_count == 0U ||
                (i > 0U && domains[i - 1U].domain_id >= domain->domain_id))
        {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0U; j < domain->method_count; ++j)
        {
            const device_link_method_descriptor_t *method =
                &domain->methods[j];

            if (method->method_id == 0U || method->permission_id == 0U ||
                    (method->channel != DEVICE_LINK_CHANNEL_SESSION &&
                     method->channel != DEVICE_LINK_CHANNEL_CONTROL) ||
                    method->handler == NULL ||
                    method->request_schema == NULL ||
                    method->response_schema == NULL ||
                    method->maximum_request_bytes >
                    DEVICE_LINK_MAX_DOMAIN_PAYLOAD_BYTES ||
                    method->maximum_response_bytes >
                    DEVICE_LINK_MAX_DOMAIN_PAYLOAD_BYTES ||
                    method->response_body_status_mask == 0U ||
                    (j > 0U && domain->methods[j - 1U].method_id >=
                     method->method_id))
            {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

esp_err_t device_link_router_init(
    device_link_router_t *router, uint64_t boot_id,
    const device_link_domain_descriptor_t *domains, size_t domain_count,
    device_link_call_replay_t *replay, size_t replay_count,
    device_link_request_digest_fn_t digest, void *digest_arg)
{
    if (router == NULL || boot_id == 0U || replay == NULL ||
            replay_count != DEVICE_LINK_REPLAY_SLOTS ||
            digest == NULL ||
            device_link_domain_descriptors_validate(domains, domain_count) !=
            ESP_OK)
    {
        return ESP_ERR_INVALID_ARG;
    }
    router->boot_id = boot_id;
    router->domains = domains;
    router->domain_count = domain_count;
    router->replay = replay;
    router->replay_count = replay_count;
    router->next_replay_slot = 0U;
    router->digest = digest;
    router->digest_arg = digest_arg;
    router->sealed = false;
    for (size_t i = 0U; i < replay_count; ++i)
    {
        if (replay[i].response == NULL || replay[i].response_capacity == 0U)
        {
            return ESP_ERR_INVALID_ARG;
        }
        device_link_call_replay_reset(&replay[i]);
    }
    return device_link_router_seal(router);
}

esp_err_t device_link_router_seal(device_link_router_t *router)
{
    if (router == NULL || router->boot_id == 0U || router->domains == NULL ||
            router->domain_count == 0U || router->replay == NULL ||
            router->replay_count != DEVICE_LINK_REPLAY_SLOTS ||
            router->digest == NULL ||
            device_link_domain_descriptors_validate(
                router->domains, router->domain_count) != ESP_OK)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < router->replay_count; ++i)
    {
        if (router->replay[i].response == NULL ||
                router->replay[i].response_capacity == 0U)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    router->sealed = true;
    return ESP_OK;
}

bool device_link_router_is_sealed(const device_link_router_t *router)
{
    return router != NULL && router->sealed;
}

void device_link_call_replay_reset(device_link_call_replay_t *replay)
{
    if (replay == NULL)
    {
        return;
    }
    _secure_zero(replay->request_digest, sizeof(replay->request_digest));
    replay->active = false;
    replay->last_call_id = 0U;
    replay->request_len = 0U;
    replay->connection_generation = 0U;
    replay->security_epoch = 0U;
    replay->domain_id = 0U;
    replay->method_id = 0U;
    if (replay->response != NULL && replay->response_capacity != 0U)
    {
        _secure_zero(replay->response, replay->response_capacity);
    }
    replay->response_len = 0U;
}

static esp_err_t _emit_status_response(
    const device_link_wire_header_t *request_header,
    device_link_status_t status, const uint8_t *payload, size_t payload_len,
    uint8_t *response, size_t capacity, size_t *response_len)
{
    const size_t prefix = DEVICE_LINK_WIRE_HEADER_BYTES +
                          DEVICE_LINK_RESPONSE_STATUS_BYTES;

    if (request_header == NULL || response == NULL || response_len == NULL ||
            (payload == NULL && payload_len != 0U) ||
            capacity < prefix || payload_len > capacity - prefix)
    {
        return ESP_ERR_NO_MEM;
    }
    device_link_wire_header_t header = *request_header;

    header.kind = DEVICE_LINK_MESSAGE_RESPONSE;
    header.recovery_query = false;
    esp_err_t result = device_link_wire_encode_header(&header, response);

    if (result == ESP_OK)
    {
        result = device_link_wire_encode_status(
                     status, &response[DEVICE_LINK_WIRE_HEADER_BYTES]);
    }
    if (result != ESP_OK)
    {
        return result;
    }
    if (payload_len != 0U)
    {
        memcpy(&response[DEVICE_LINK_WIRE_HEADER_BYTES +
                         DEVICE_LINK_RESPONSE_STATUS_BYTES],
               payload, payload_len);
    }
    *response_len = DEVICE_LINK_WIRE_HEADER_BYTES +
                    DEVICE_LINK_RESPONSE_STATUS_BYTES + payload_len;
    return ESP_OK;
}

static esp_err_t _check_replay(
    const device_link_call_replay_t *replay, size_t replay_count,
    const device_link_request_context_t *facts,
    device_link_request_digest_fn_t digest, void *digest_arg,
    const device_link_wire_header_t *header,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    bool *handled)
{
    *handled = false;
    uint8_t request_digest[DEVICE_LINK_REPLAY_DIGEST_BYTES];
    esp_err_t digest_result = digest(request, request_len, request_digest,
                                     digest_arg);
    if (digest_result != ESP_OK)
    {
        _secure_zero(request_digest, sizeof(request_digest));
        return digest_result;
    }
    const device_link_call_replay_t *context = NULL;
    for (size_t i = 0U; i < replay_count; ++i)
    {
        const device_link_call_replay_t *candidate = &replay[i];
        if (!candidate->active ||
                facts->connection_generation != candidate->connection_generation ||
                facts->security_epoch != candidate->security_epoch)
        {
            continue;
        }
        context = candidate;
        break;
    }
    if (context != NULL && header->call_id == context->last_call_id)
    {
        *handled = true;
        if (header->domain_id == context->domain_id &&
                header->method_id == context->method_id &&
                request_len == context->request_len &&
                memcmp(request_digest, context->request_digest,
                       sizeof(request_digest)) == 0)
        {
            if (context->response_len > response_capacity)
            {
                _secure_zero(request_digest, sizeof(request_digest));
                return ESP_ERR_NO_MEM;
            }
            memcpy(response, context->response, context->response_len);
            *response_len = context->response_len;
            _secure_zero(request_digest, sizeof(request_digest));
            return ESP_OK;
        }
        _secure_zero(request_digest, sizeof(request_digest));
        return _emit_status_response(
                   header, DEVICE_LINK_STATUS_CONFLICT, NULL, 0U,
                   response, response_capacity, response_len);
    }
    if ((context == NULL && header->call_id != 1U) ||
            (context != NULL &&
             (context->last_call_id == UINT32_MAX ||
              header->call_id != context->last_call_id + 1U)))
    {
        *handled = true;
        _secure_zero(request_digest, sizeof(request_digest));
        return _emit_status_response(
                   header, DEVICE_LINK_STATUS_CONFLICT, NULL, 0U,
                   response, response_capacity, response_len);
    }
    _secure_zero(request_digest, sizeof(request_digest));
    return ESP_OK;
}

static size_t _select_replay_slot(
    const device_link_router_t *router,
    const device_link_request_context_t *facts)
{
    for (size_t i = 0U; i < router->replay_count; ++i)
    {
        if (router->replay[i].active &&
                router->replay[i].connection_generation ==
                facts->connection_generation &&
                router->replay[i].security_epoch == facts->security_epoch)
        {
            return i;
        }
    }
    for (size_t i = 0U; i < router->replay_count; ++i)
    {
        const size_t candidate = (router->next_replay_slot + i) %
                                 router->replay_count;

        if (!router->replay[candidate].active)
        {
            return candidate;
        }
    }
    return router->next_replay_slot;
}

static device_link_status_t _admit_method(
    const device_link_request_context_t *facts,
    const device_link_wire_header_t *header,
    const device_link_method_descriptor_t *method)
{
    if (!facts->security_authenticated)
    {
        return DEVICE_LINK_STATUS_UNAUTHENTICATED;
    }
    if (facts->channel != method->channel)
    {
        return DEVICE_LINK_STATUS_UNSUPPORTED_OPERATION;
    }
    if (header->recovery_query &&
            !(header->domain_id == DEVICE_LINK_DOMAIN_CORE &&
              (header->method_id == 5U || header->method_id == 6U)))
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    if (facts->authorized)
    {
        return device_link_permission_contains(
                   facts->permissions, facts->permission_count,
                   method->permission_id) ? DEVICE_LINK_STATUS_OK :
               DEVICE_LINK_STATUS_PERMISSION_DENIED;
    }
    if (facts->channel != DEVICE_LINK_CHANNEL_SESSION ||
            header->domain_id != DEVICE_LINK_DOMAIN_CORE)
    {
        return DEVICE_LINK_STATUS_UNAUTHENTICATED;
    }
    if (header->method_id == 1U || header->method_id == 2U)
    {
        return DEVICE_LINK_STATUS_OK;
    }
    if (facts->admission == DEVICE_LINK_ADMISSION_BOUND_PUBLIC_READ_ONLY)
    {
        return header->method_id == 3U ?
               DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED :
               DEVICE_LINK_STATUS_UNAUTHENTICATED;
    }
    if (header->method_id == 3U || header->method_id == 4U)
    {
        return DEVICE_LINK_STATUS_OK;
    }
    if (header->method_id == 5U && header->recovery_query)
    {
        return DEVICE_LINK_STATUS_OK;
    }
    return DEVICE_LINK_STATUS_UNAUTHENTICATED;
}

esp_err_t device_link_router_process(
    device_link_router_t *router,
    const device_link_request_context_t *facts,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len)
{
    if (router == NULL || facts == NULL || request == NULL ||
            response == NULL || response_len == NULL ||
            !router->sealed ||
            request_len < DEVICE_LINK_WIRE_HEADER_BYTES ||
            (facts->channel != DEVICE_LINK_CHANNEL_SESSION &&
             facts->channel != DEVICE_LINK_CHANNEL_CONTROL) ||
            (facts->admission < DEVICE_LINK_ADMISSION_UNBOUND_PUBLIC ||
             facts->admission > DEVICE_LINK_ADMISSION_AUTHORIZED) ||
            !device_link_permission_set_valid(
                facts->permissions, facts->permission_count))
    {
        return ESP_ERR_INVALID_ARG;
    }
    device_link_wire_header_t header;
    esp_err_t result = device_link_wire_decode_header(
                           request, request_len, &header);

    if (result != ESP_OK)
    {
        return result;
    }
    if (header.kind != DEVICE_LINK_MESSAGE_REQUEST)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (header.boot_id != router->boot_id)
    {
        return _emit_status_response(
                   &header, DEVICE_LINK_STATUS_CONFLICT, NULL, 0U,
                   response, response_capacity, response_len);
    }
    bool handled = false;

    result = _check_replay(router->replay, router->replay_count, facts,
                           router->digest, router->digest_arg, &header,
                           request, request_len,
                           response, response_capacity, response_len,
                           &handled);
    if (result != ESP_OK || handled)
    {
        return result;
    }
    if (request_len > DEVICE_LINK_MAX_MESSAGE_BYTES)
    {
        return _emit_status_response(
                   &header, DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED, NULL, 0U,
                   response, response_capacity, response_len);
    }
    const device_link_domain_descriptor_t *domain =
        _find_domain(router, header.domain_id);
    const device_link_method_descriptor_t *method = NULL;
    device_link_status_t status = DEVICE_LINK_STATUS_OK;

    if (domain == NULL)
    {
        status = DEVICE_LINK_STATUS_UNSUPPORTED_CAPABILITY;
    }
    else if (domain->major != header.domain_major)
    {
        status = DEVICE_LINK_STATUS_UNSUPPORTED_VERSION;
    }
    else
    {
        method = _find_method(domain, header.method_id);
        if (method == NULL)
        {
            status = DEVICE_LINK_STATUS_UNSUPPORTED_OPERATION;
        }
    }
    const size_t payload_len = request_len - DEVICE_LINK_WIRE_HEADER_BYTES;

    if (status == DEVICE_LINK_STATUS_OK &&
            payload_len > method->maximum_request_bytes)
    {
        status = DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    if (status == DEVICE_LINK_STATUS_OK &&
            device_link_tlv_validate_message(
                &request[DEVICE_LINK_WIRE_HEADER_BYTES], payload_len,
                method->request_schema) != ESP_OK)
    {
        status = DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    if (status == DEVICE_LINK_STATUS_OK)
    {
        status = _admit_method(facts, &header, method);
    }
    size_t body_len = 0U;

    if (status == DEVICE_LINK_STATUS_OK)
    {
        const size_t prefix = DEVICE_LINK_WIRE_HEADER_BYTES +
                              DEVICE_LINK_RESPONSE_STATUS_BYTES;
        const size_t slot_index = _select_replay_slot(router, facts);

        if (response_capacity < prefix ||
                router->replay[slot_index].response_capacity < prefix)
        {
            status = DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
        }
        else
        {
            size_t handler_capacity = response_capacity - prefix;

            if (handler_capacity >
                    router->replay[slot_index].response_capacity - prefix)
            {
                handler_capacity =
                    router->replay[slot_index].response_capacity - prefix;
            }
            if (handler_capacity > method->maximum_response_bytes)
            {
                handler_capacity = method->maximum_response_bytes;
            }
            device_link_request_context_t context = *facts;

            context.header = header;
            status = method->handler(
                         &context,
                         &request[DEVICE_LINK_WIRE_HEADER_BYTES], payload_len,
                         &response[prefix],
                         handler_capacity,
                         &body_len,
                         method->handler_arg);
            if (status < DEVICE_LINK_STATUS_OK ||
                    status > DEVICE_LINK_STATUS_INTERNAL ||
                    body_len > method->maximum_response_bytes)
            {
                status = DEVICE_LINK_STATUS_INTERNAL;
                body_len = 0U;
            }
            else if ((method->response_body_status_mask &
                      DEVICE_LINK_STATUS_MASK(status)) == 0U &&
                     body_len != 0U)
            {
                status = DEVICE_LINK_STATUS_INTERNAL;
                body_len = 0U;
            }
            else if ((method->response_body_status_mask &
                      DEVICE_LINK_STATUS_MASK(status)) != 0U &&
                     device_link_tlv_validate_message(
                         &response[prefix], body_len,
                         method->response_schema) != ESP_OK)
            {
                status = DEVICE_LINK_STATUS_INTERNAL;
                body_len = 0U;
            }
        }
    }
    result = _emit_status_response(
                 &header, status,
                 body_len != 0U ?
                 &response[DEVICE_LINK_WIRE_HEADER_BYTES +
                           DEVICE_LINK_RESPONSE_STATUS_BYTES] : NULL,
                 body_len,
                 response, response_capacity, response_len);
    if (result != ESP_OK)
    {
        return result;
    }
    const size_t slot_index = _select_replay_slot(router, facts);
    device_link_call_replay_t *replay = &router->replay[slot_index];
    if (*response_len > replay->response_capacity)
    {
        return ESP_ERR_NO_MEM;
    }
    result = router->digest(request, request_len,
                            replay->request_digest,
                            router->digest_arg);
    if (result != ESP_OK)
    {
        return result;
    }
    memcpy(replay->response, response, *response_len);
    replay->active = true;
    replay->request_len = request_len;
    replay->response_len = *response_len;
    replay->last_call_id = header.call_id;
    replay->connection_generation = facts->connection_generation;
    replay->security_epoch = facts->security_epoch;
    replay->domain_id = header.domain_id;
    replay->method_id = header.method_id;
    router->next_replay_slot = (slot_index + 1U) % router->replay_count;
    return ESP_OK;
}

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ble_link_service.h"
#include "connectivity_manager.h"
#include "device_link_protocol.h"
#include "device_link_tlv.h"
#include "device_link_wifi_adapter.h"
#include "event_bus.h"
#include "mt_log.h"

#define WIFI_METHOD_GET_STATUS 1U
#define WIFI_METHOD_START_SCAN 2U
#define WIFI_METHOD_GET_SCAN_RESULTS 3U
#define WIFI_METHOD_SET_CREDENTIALS 4U
#define WIFI_METHOD_DISCONNECT 5U
#define WIFI_METHOD_RECONNECT_SAVED 6U
#define WIFI_METHOD_FORGET_SAVED 7U
#define WIFI_METHOD_SET_AUTO_CONNECT 8U

static const uint64_t s_security_values[] = {1U, 2U, 3U};
static const uint64_t s_state_values[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
static const uint64_t s_failure_values[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};

static const device_link_tlv_schema_t s_empty_schema =
{
    .fields = NULL,
    .field_count = 0U,
    .maximum_encoded_bytes = 0U,
};

static const device_link_tlv_field_rule_t s_credentials_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
        .minimum_bytes = 1U, .maximum_bytes = 32U,
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
        .maximum_bytes = 64U,
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
        .maximum_unsigned = 3U, .enum_values = s_security_values,
        .enum_count = 3U,
    },
};
static const device_link_tlv_schema_t s_credentials_schema =
{
    .fields = s_credentials_fields,
    .field_count = 3U,
    .maximum_encoded_bytes = 128U,
};

static const device_link_tlv_field_rule_t s_status_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_NONZERO,
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 8U,
        .enum_values = s_state_values, .enum_count = 8U,
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 9U,
        .enum_values = s_failure_values, .enum_count = 9U,
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .maximum_bytes = 32U,
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
    {
        .id = 6U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
    {
        .id = 7U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
    {
        .id = 8U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
    {
        .id = 9U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
    {
        .id = 10U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_NONZERO,
    },
    {
        .id = 11U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_NONZERO,
    },
};
/* WifiStatus operation result payloads never exceed this bound: it must
 * stay equal to s_status_schema.maximum_encoded_bytes. A 256-byte stack
 * buffer keeps the completion bridge out of the 3 KB operation-result
 * cap, which would otherwise be an unnecessary 3000-byte frame on the
 * connectivity publisher stack. */
#define WIFI_STATUS_RESULT_MAX_BYTES 256U

static const device_link_tlv_schema_t s_status_schema =
{
    .fields = s_status_fields, .field_count = 11U,
    .maximum_encoded_bytes = WIFI_STATUS_RESULT_MAX_BYTES,
};

static const device_link_tlv_field_rule_t s_network_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
        .minimum_bytes = 1U, .maximum_bytes = 32U,
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_SIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
        .minimum_signed = -127, .maximum_signed = 20,
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
        .minimum_unsigned = 1U, .maximum_unsigned = 14U,
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
        .maximum_unsigned = 3U, .enum_values = s_security_values,
        .enum_count = 3U,
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
};
static const device_link_tlv_schema_t s_network_schema =
{
    .fields = s_network_fields, .field_count = 5U,
    .maximum_encoded_bytes = 64U,
};

static const device_link_tlv_field_rule_t s_start_scan_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
};
static const device_link_tlv_schema_t s_start_scan_schema =
{
    .fields = s_start_scan_fields, .field_count = 1U,
    .maximum_encoded_bytes = 8U,
};

static const device_link_tlv_field_rule_t s_scan_query_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_NONZERO,
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 31U,
    },
};
static const device_link_tlv_schema_t s_scan_query_schema =
{
    .fields = s_scan_query_fields, .field_count = 2U,
    .maximum_encoded_bytes = 32U,
};

static const device_link_tlv_field_rule_t s_scan_results_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_NONZERO,
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 31U,
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REPEATED | DEVICE_LINK_TLV_RULE_MESSAGE,
        .maximum_count = 8U, .nested = &s_network_schema,
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
};
static const device_link_tlv_schema_t s_scan_results_schema =
{
    .fields = s_scan_results_fields, .field_count = 5U,
    .maximum_encoded_bytes = 768U,
};

static const device_link_tlv_field_rule_t s_set_credentials_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_MESSAGE,
        .nested = &s_credentials_schema,
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_NONZERO,
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
};
static const device_link_tlv_schema_t s_set_credentials_schema =
{
    .fields = s_set_credentials_fields, .field_count = 3U,
    .maximum_encoded_bytes = 160U,
};

static const device_link_tlv_field_rule_t s_auto_connect_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U,
    },
};
static const device_link_tlv_schema_t s_auto_connect_schema =
{
    .fields = s_auto_connect_fields, .field_count = 1U,
    .maximum_encoded_bytes = 8U,
};

static const device_link_tlv_field_rule_t s_operation_accepted_field =
{
    .id = 1U,
    .wire_type = DEVICE_LINK_TLV_FIXED64,
    .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_NONZERO,
};

static const device_link_tlv_schema_t s_operation_accepted_schema =
{
    .fields = &s_operation_accepted_field,
    .field_count = 1U,
    .maximum_encoded_bytes = 16U,
};

static device_link_status_t _map_result(esp_err_t result)
{
    if (result == ESP_OK)
    {
        return DEVICE_LINK_STATUS_OK;
    }
    if (result == ESP_ERR_INVALID_ARG)
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    if (result == ESP_ERR_NO_MEM)
    {
        return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    if (result == ESP_ERR_NOT_FOUND)
    {
        return DEVICE_LINK_STATUS_NOT_FOUND;
    }
    if (result == ESP_ERR_INVALID_STATE)
    {
        /* The manager lifecycle is not running (cold-start window or
         * shutdown). Core v2 maps a disabled/unavailable owner to
         * UNAVAILABLE; CONFLICT is reserved for duplicate-request replay
         * and client-sync conflicts. */
        return DEVICE_LINK_STATUS_UNAVAILABLE;
    }
    if (result == ESP_ERR_TIMEOUT)
    {
        return DEVICE_LINK_STATUS_UNAVAILABLE;
    }
    return DEVICE_LINK_STATUS_INTERNAL;
}

/**
 * @brief Map an admission failure to the statuses frozen in the Wi-Fi v1
 * method allowed_statuses.
 *
 * RESOURCE_EXHAUSTED is allowed only for start_scan and set_credentials;
 * the other asynchronous methods express table/queue exhaustion as
 * UNAVAILABLE (their allowed sets do not contain RESOURCE_EXHAUSTED).
 */
static device_link_status_t _map_admission_result(
    uint8_t method_id, esp_err_t result)
{
    if (result != ESP_ERR_NO_MEM ||
            method_id == WIFI_METHOD_START_SCAN ||
            method_id == WIFI_METHOD_SET_CREDENTIALS)
    {
        return _map_result(result);
    }
    return DEVICE_LINK_STATUS_UNAVAILABLE;
}

static bool _read_message(const uint8_t *request, size_t request_len,
                          const device_link_tlv_schema_t *schema,
                          device_link_tlv_reader_t *reader)
{
    return request != NULL && reader != NULL &&
           device_link_tlv_validate_message(request, request_len, schema) ==
           ESP_OK && device_link_tlv_reader_init(reader, request, request_len) ==
           ESP_OK;
}

static bool _read_bool(const uint8_t *request, size_t request_len,
                       const device_link_tlv_schema_t *schema, bool *value)
{
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has = false;

    if (value == NULL || !_read_message(request, request_len, schema, &reader))
    {
        return false;
    }
    if (device_link_tlv_reader_next(&reader, &field, &has) != ESP_OK || !has)
    {
        return false;
    }
    *value = field.value.unsigned_value != 0U;
    return device_link_tlv_reader_next(&reader, &field, &has) == ESP_OK &&
           !has && reader.offset == reader.len;
}

static device_link_status_t _encode_operation_status(
    uint8_t method_id, device_link_status_t status, uint64_t operation_id,
    uint8_t *response, size_t capacity, size_t *response_len)
{
    device_link_tlv_writer_t writer;

    if (response == NULL || response_len == NULL)
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    *response_len = 0U;
    if (status != DEVICE_LINK_STATUS_OK)
    {
        /* Admission failures have no operation identity to expose. */
        return status;
    }
    if (operation_id == 0U)
    {
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    device_link_tlv_writer_init(&writer, response, capacity);
    (void)method_id;
    if (device_link_tlv_put_fixed64(&writer, 1U, operation_id) != ESP_OK ||
            device_link_tlv_writer_finish(&writer, response_len) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    return DEVICE_LINK_STATUS_OK;
}

static device_link_status_t _encode_operation(
    uint8_t method_id, esp_err_t result, uint64_t operation_id,
    uint8_t *response, size_t capacity, size_t *response_len)
{
    return _encode_operation_status(
               method_id, _map_admission_result(method_id, result),
               operation_id, response, capacity, response_len);
}

static device_link_status_t _encode_status(
    const connectivity_manager_status_snapshot_t *status,
    uint8_t *response, size_t capacity, size_t *response_len)
{
    device_link_tlv_writer_t writer;

    if (status == NULL || response == NULL || response_len == NULL ||
            status->generation == 0U || status->profile_revision == 0U)
    {
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    device_link_tlv_writer_init(&writer, response, capacity);
    if (device_link_tlv_put_fixed64(&writer, 1U, status->generation) != ESP_OK ||
            device_link_tlv_put_uint(&writer, 2U,
                                     (uint64_t)status->state + 1U) != ESP_OK ||
            device_link_tlv_put_uint(&writer, 3U,
                                     (uint64_t)status->failure + 1U) != ESP_OK ||
            (status->ssid[0] != '\0' && device_link_tlv_put_bytes(
                 &writer, 4U, (const uint8_t *)status->ssid,
                 strlen(status->ssid)) != ESP_OK) ||
            device_link_tlv_put_bool(&writer, 5U, status->ipv4_address != 0U) !=
            ESP_OK || device_link_tlv_put_bool(&writer, 6U,
                    status->saved_profile) != ESP_OK ||
            device_link_tlv_put_bool(&writer, 7U,
                                     status->profile_persisted) != ESP_OK ||
            device_link_tlv_put_bool(&writer, 8U, status->auto_connect) !=
            ESP_OK || device_link_tlv_put_bool(&writer, 9U, status->manual_hold) !=
            ESP_OK || device_link_tlv_put_fixed64(&writer, 10U,
                    status->profile_revision) !=
            ESP_OK || (status->applied_client_sync_id != 0U &&
                       device_link_tlv_put_fixed64(&writer, 11U,
                               status->applied_client_sync_id) != ESP_OK) ||
            device_link_tlv_writer_finish(&writer, response_len) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    return DEVICE_LINK_STATUS_OK;
}

/**
 * @brief Cancel the lower-layer operation behind a Core operation id.
 *
 * Invoked by CancelOperation through the operation table.
 */
static esp_err_t _wifi_operation_cancel(uint64_t owner_id, void *arg)
{
    (void)arg;
    return connectivity_manager_cancel(
               (connectivity_manager_operation_id_t)owner_id);
}

/**
 * @brief Admit an accepted asynchronous manager operation into the Core
 * v2 operation table.
 *
 * The table id (not the manager id) is encoded into OperationAccepted and
 * drives GetOperation/CancelOperation. The manager id is retained as the
 * table owner id so the completion bridge can match terminal snapshots.
 * An admission failure of the manager request is mapped without an
 * operation identity.
 */
static device_link_status_t _admit_async_operation(
    uint8_t method_id, esp_err_t manager_result,
    connectivity_manager_operation_id_t manager_operation_id,
    uint8_t *response, size_t response_capacity, size_t *response_len)
{
    if (manager_result != ESP_OK)
    {
        return _encode_operation(method_id, manager_result, 0U, response,
                                 response_capacity, response_len);
    }
    uint64_t table_operation_id = 0U;
    const esp_err_t admit_result = ble_link_service_async_operation_start(
                                       DEVICE_LINK_DOMAIN_WIFI, method_id,
                                       (uint64_t)manager_operation_id,
                                       _wifi_operation_cancel, NULL,
                                       &table_operation_id);

    if (admit_result != ESP_OK)
    {
        /* The manager admitted the work but the Core table is full or
         * unavailable: cancel the manager side and report exhaustion. */
        (void)connectivity_manager_cancel(manager_operation_id);
        return _encode_operation(method_id, ESP_ERR_NO_MEM, 0U, response,
                                 response_capacity, response_len);
    }
    return _encode_operation(method_id, ESP_OK, table_operation_id,
                             response, response_capacity, response_len);
}

static device_link_status_t _wifi_handler(
    const device_link_request_context_t *context,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    void *arg)
{
    (void)arg;
    if (context == NULL || response == NULL || response_len == NULL ||
            !context->authorized)
    {
        return DEVICE_LINK_STATUS_PERMISSION_DENIED;
    }
    const uint8_t method = context->header.method_id;
    connectivity_manager_operation_id_t operation_id = 0U;
    esp_err_t result;

    if ((method == WIFI_METHOD_START_SCAN ||
            method == WIFI_METHOD_DISCONNECT ||
            method == WIFI_METHOD_RECONNECT_SAVED) &&
            ble_link_service_async_operation_in_flight(
                DEVICE_LINK_DOMAIN_WIFI))
    {
        /* The Wi-Fi v1 allowed_statuses freeze BUSY for these methods when
         * another operation is still live; the lower layer would otherwise
         * silently defer the command and report the conflict only as a
         * late terminal event. */
        return _encode_operation_status(
                   method, DEVICE_LINK_STATUS_BUSY, 0U, response,
                   response_capacity, response_len);
    }
    if (method == WIFI_METHOD_GET_STATUS)
    {
        connectivity_manager_status_snapshot_t status;

        if (request_len != 0U || connectivity_manager_get_status(&status) !=
                ESP_OK)
        {
            return DEVICE_LINK_STATUS_UNAVAILABLE;
        }
        return _encode_status(&status, response, response_capacity,
                              response_len);
    }
    if (method == WIFI_METHOD_START_SCAN)
    {
        /* The optional force_refresh field is accepted and validated by
         * the schema. The connectivity manager has no scan cache: every
         * request starts a fresh scan, so the "always refresh" behavior
         * is a strict superset of force_refresh=false semantics. The field
         * is decoded so the wire value is never silently dropped. */
        bool force_refresh = false;

        if (request_len != 0U &&
                !_read_bool(request, request_len, &s_start_scan_schema,
                            &force_refresh))
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        (void)force_refresh;
        result = connectivity_manager_request_scan(&operation_id);
        return _admit_async_operation(method, result, operation_id,
                                      response, response_capacity,
                                      response_len);
    }
    if (method == WIFI_METHOD_GET_SCAN_RESULTS)
    {
        device_link_tlv_reader_t reader;
        device_link_tlv_field_t field;
        bool has = false;
        uint64_t generation = 0U;
        uint64_t page = 0U;

        if (!_read_message(request, request_len, &s_scan_query_schema, &reader))
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        while (device_link_tlv_reader_next(&reader, &field, &has) == ESP_OK &&
                has)
        {
            if (field.id == 1U)
            {
                generation = field.value.fixed64_value;
            }
            else if (field.id == 2U)
            {
                page = field.value.unsigned_value;
            }
        }
        if (reader.offset != reader.len)
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        connectivity_manager_scan_snapshot_t scan;

        result = connectivity_manager_get_scan_snapshot(&scan);
        if (result != ESP_OK || scan.generation != generation)
        {
            return result == ESP_OK ? DEVICE_LINK_STATUS_NOT_FOUND :
                   DEVICE_LINK_STATUS_UNAVAILABLE;
        }
        device_link_tlv_writer_t writer;

        device_link_tlv_writer_init(&writer, response, response_capacity);
        if (device_link_tlv_put_fixed64(&writer, 1U, scan.generation) != ESP_OK ||
                device_link_tlv_put_uint(&writer, 2U, page) != ESP_OK)
        {
            return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
        }
        /* Pages of at most eight networks; has_more reflects the tail. */
        const size_t page_size = 8U;
        const size_t first = (size_t)page * page_size;

        if (first >= scan.record_count)
        {
            /* The requested page is past the end of the snapshot. */
            if (device_link_tlv_put_bool(&writer, 4U, false) != ESP_OK ||
                    device_link_tlv_put_bool(&writer, 5U,
                                             scan.truncated) != ESP_OK ||
                    device_link_tlv_writer_finish(&writer, response_len) !=
                    ESP_OK)
            {
                return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
            }
            return DEVICE_LINK_STATUS_OK;
        }
        const size_t last = first + page_size < scan.record_count ?
                            first + page_size : scan.record_count;

        for (size_t i = first; i < last; ++i)
        {
            uint8_t network[64];
            device_link_tlv_writer_t nested;
            size_t network_len = 0U;

            device_link_tlv_writer_init(&nested, network, sizeof(network));
            if (device_link_tlv_put_bytes(&nested, 1U,
                                          (const uint8_t *)scan.records[i].ssid,
                                          strlen(scan.records[i].ssid)) != ESP_OK ||
                    device_link_tlv_put_sint(&nested, 2U,
                                             scan.records[i].rssi) != ESP_OK ||
                    device_link_tlv_put_uint(&nested, 3U,
                                             scan.records[i].channel) != ESP_OK ||
                    device_link_tlv_put_uint(&nested, 4U,
                                             (uint64_t)scan.records[i].security + 1U) != ESP_OK ||
                    device_link_tlv_put_bool(&nested, 5U,
                                             scan.records[i].saved) != ESP_OK ||
                    device_link_tlv_writer_finish(&nested, &network_len) !=
                    ESP_OK || device_link_tlv_put_bytes(&writer, 3U, network,
                            network_len) != ESP_OK)
            {
                return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
            }
        }
        const bool has_more = last < scan.record_count;

        if (device_link_tlv_put_bool(&writer, 4U, has_more) != ESP_OK ||
                device_link_tlv_put_bool(&writer, 5U, scan.truncated) != ESP_OK ||
                device_link_tlv_writer_finish(&writer, response_len) != ESP_OK)
        {
            return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
        }
        return DEVICE_LINK_STATUS_OK;
    }
    if (method == WIFI_METHOD_SET_CREDENTIALS)
    {
        device_link_tlv_reader_t reader;
        device_link_tlv_field_t field;
        bool has = false;
        uint64_t sync_id = 0U;
        bool auto_connect = false;
        const uint8_t *nested_data = NULL;
        size_t nested_len = 0U;

        if (!_read_message(request, request_len, &s_set_credentials_schema,
                           &reader))
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        while (device_link_tlv_reader_next(&reader, &field, &has) == ESP_OK &&
                has)
        {
            if (field.id == 1U)
            {
                nested_data = field.value.bytes.data;
                nested_len = field.value.bytes.len;
            }
            else if (field.id == 2U)
            {
                sync_id = field.value.fixed64_value;
            }
            else if (field.id == 3U)
            {
                auto_connect = field.value.unsigned_value != 0U;
            }
        }
        if (reader.offset != reader.len || nested_data == NULL)
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        device_link_tlv_reader_t nested;
        const char *ssid = NULL;
        const char *password = NULL;
        size_t ssid_len = 0U;
        size_t password_len = 0U;
        uint64_t security = 0U;

        if (!_read_message(nested_data, nested_len, &s_credentials_schema,
                           &nested))
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        while (device_link_tlv_reader_next(&nested, &field, &has) == ESP_OK &&
                has)
        {
            if (field.id == 1U)
            {
                ssid = (const char *)field.value.bytes.data;
                ssid_len = field.value.bytes.len;
            }
            else if (field.id == 2U)
            {
                password = (const char *)field.value.bytes.data;
                password_len = field.value.bytes.len;
            }
            else if (field.id == 3U)
            {
                security = field.value.unsigned_value;
            }
        }
        if (nested.offset != nested.len || ssid == NULL || password == NULL)
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        const connectivity_manager_credentials_t credentials =
        {
            .ssid = ssid,
            .ssid_length = ssid_len,
            .password = password,
            .password_length = password_len,
            .security = (connectivity_manager_security_t)(security - 1U),
        };
        result = connectivity_manager_request_sync_profile(
                     &credentials, sync_id, auto_connect, &operation_id);
        return _admit_async_operation(method, result, operation_id,
                                      response, response_capacity,
                                      response_len);
    }
    if (method == WIFI_METHOD_DISCONNECT)
    {
        if (request_len != 0U)
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        result = connectivity_manager_request_disconnect(&operation_id);
    }
    else if (method == WIFI_METHOD_RECONNECT_SAVED)
    {
        if (request_len != 0U)
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        result = connectivity_manager_request_reconnect_saved(&operation_id);
    }
    else if (method == WIFI_METHOD_FORGET_SAVED)
    {
        if (request_len != 0U)
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        result = connectivity_manager_request_forget(&operation_id);
    }
    else if (method == WIFI_METHOD_SET_AUTO_CONNECT)
    {
        bool enabled = false;

        if (!_read_bool(request, request_len, &s_auto_connect_schema, &enabled))
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        result = connectivity_manager_set_auto_connect(enabled, &operation_id);
    }
    else
    {
        return DEVICE_LINK_STATUS_UNSUPPORTED_OPERATION;
    }
    return _admit_async_operation(method, result, operation_id,
                                  response, response_capacity,
                                  response_len);
}

static const device_link_method_descriptor_t s_methods[] =
{
    {
        .method_id = WIFI_METHOD_GET_STATUS,
        .channel = DEVICE_LINK_CHANNEL_CONTROL,
        .permission_id = DEVICE_LINK_PERMISSION_WIFI_READ,
        .maximum_response_bytes = 256U,
        .request_schema = &s_empty_schema,
        .response_schema = &s_status_schema,
        .response_body_status_mask = DEVICE_LINK_STATUS_MASK(
            DEVICE_LINK_STATUS_OK),
        .handler = _wifi_handler,
    },
    {
        .method_id = WIFI_METHOD_START_SCAN,
        .flags = DEVICE_LINK_METHOD_ASYNCHRONOUS,
        .channel = DEVICE_LINK_CHANNEL_CONTROL,
        .permission_id = DEVICE_LINK_PERMISSION_WIFI_SCAN,
        .maximum_request_bytes = 8U,
        .maximum_response_bytes = 16U,
        .request_schema = &s_start_scan_schema,
        .response_schema = &s_operation_accepted_schema,
        /* start_scan declares core.v2.Empty: a SUCCEEDED record carries no
         * result payload; scan pages are read via GetScanResults. */
        .operation_result_schema = &s_empty_schema,
        .response_body_status_mask = DEVICE_LINK_STATUS_MASK(
            DEVICE_LINK_STATUS_OK),
        .handler = _wifi_handler,
    },
    {
        .method_id = WIFI_METHOD_GET_SCAN_RESULTS,
        .channel = DEVICE_LINK_CHANNEL_CONTROL,
        .permission_id = DEVICE_LINK_PERMISSION_WIFI_READ,
        .maximum_request_bytes = 32U,
        .maximum_response_bytes = 768U,
        .request_schema = &s_scan_query_schema,
        .response_schema = &s_scan_results_schema,
        .response_body_status_mask = DEVICE_LINK_STATUS_MASK(
            DEVICE_LINK_STATUS_OK),
        .handler = _wifi_handler,
    },
#define WIFI_ASYNC_METHOD(id, permission, request_limit, request_type) \
    { \
        .method_id = (id), \
        .flags = DEVICE_LINK_METHOD_ASYNCHRONOUS, \
        .channel = DEVICE_LINK_CHANNEL_CONTROL, \
        .permission_id = (permission), \
        .maximum_request_bytes = (request_limit), \
        .maximum_response_bytes = 16U, \
        .request_schema = (request_type), \
        .response_schema = &s_operation_accepted_schema, \
        .operation_result_schema = &s_status_schema, \
        .response_body_status_mask = DEVICE_LINK_STATUS_MASK( \
                                         DEVICE_LINK_STATUS_OK), \
        .handler = _wifi_handler, \
    }
    WIFI_ASYNC_METHOD(WIFI_METHOD_SET_CREDENTIALS,
                      DEVICE_LINK_PERMISSION_WIFI_WRITE, 160U,
                      &s_set_credentials_schema),
    WIFI_ASYNC_METHOD(WIFI_METHOD_DISCONNECT,
                      DEVICE_LINK_PERMISSION_WIFI_WRITE, 0U, &s_empty_schema),
    WIFI_ASYNC_METHOD(WIFI_METHOD_RECONNECT_SAVED,
                      DEVICE_LINK_PERMISSION_WIFI_WRITE, 0U, &s_empty_schema),
    WIFI_ASYNC_METHOD(WIFI_METHOD_FORGET_SAVED,
                      DEVICE_LINK_PERMISSION_WIFI_WRITE, 0U, &s_empty_schema),
    WIFI_ASYNC_METHOD(WIFI_METHOD_SET_AUTO_CONNECT,
                      DEVICE_LINK_PERMISSION_WIFI_WRITE, 8U,
                      &s_auto_connect_schema),
#undef WIFI_ASYNC_METHOD
};

static const device_link_domain_descriptor_t s_descriptor =
{
    .domain_id = DEVICE_LINK_DOMAIN_WIFI,
    .major = 1U,
    .minor = 0U,
    .methods = s_methods,
    .method_count = sizeof(s_methods) / sizeof(s_methods[0]),
};

esp_err_t device_link_wifi_adapter_get_descriptor(
    const device_link_domain_descriptor_t **descriptor)
{
    if (descriptor == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *descriptor = &s_descriptor;
    return ESP_OK;
}

esp_err_t device_link_wifi_adapter_encode_operation_result(
    const connectivity_manager_status_snapshot_t *status,
    uint8_t *response, size_t capacity, size_t *response_len)
{
    const device_link_status_t encode_result =
        _encode_status(status, response, capacity, response_len);

    return encode_result == DEVICE_LINK_STATUS_OK ? ESP_OK :
           ESP_ERR_INVALID_ARG;
}

/* ---------------------------------------------------------------------------
 * Completion bridge.
 *
 * Terminal connectivity manager snapshots are forwarded into the Core v2
 * operation table as OperationStatus transitions: the manager operation id
 * that was recorded as the table owner id at admission is matched back
 * through find_by_owner, and the terminal WifiStatus payload is attached to
 * SUCCEEDED records of result-declaring methods. The callback runs in the
 * connectivity publisher context and only touches the link service table
 * lock, so it can never wait on the manager worker.
 * ------------------------------------------------------------------------- */

static event_bus_sub_handle_t s_bridge_handle = EVENT_BUS_SUB_HANDLE_INVALID;

/**
 * @brief Map a classified manager failure to the LinkError taxonomy.
 *
 * AUTHENTICATION is a client-credential rejection (PERMISSION_DENIED),
 * AP_NOT_FOUND maps to NOT_FOUND, transient radio/network conditions map
 * to UNAVAILABLE, and the remaining classes carry their obvious LinkError
 * counterparts. The mapping is total: an unknown classification degrades
 * to INTERNAL.
 */
static device_link_status_t _bridge_map_failure(
    connectivity_manager_failure_t failure)
{
    switch (failure)
    {
    case CONNECTIVITY_MANAGER_FAILURE_NONE:
        return DEVICE_LINK_STATUS_OK;
    case CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION:
        return DEVICE_LINK_STATUS_PERMISSION_DENIED;
    case CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND:
        return DEVICE_LINK_STATUS_NOT_FOUND;
    case CONNECTIVITY_MANAGER_FAILURE_ASSOCIATION_TIMEOUT:
    case CONNECTIVITY_MANAGER_FAILURE_DHCP_TIMEOUT:
    case CONNECTIVITY_MANAGER_FAILURE_LINK_LOST:
    case CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE:
        return DEVICE_LINK_STATUS_UNAVAILABLE;
    case CONNECTIVITY_MANAGER_FAILURE_STORAGE:
        return DEVICE_LINK_STATUS_STORAGE;
    case CONNECTIVITY_MANAGER_FAILURE_CONFLICT:
        return DEVICE_LINK_STATUS_CONFLICT;
    case CONNECTIVITY_MANAGER_FAILURE_INTERNAL:
        return DEVICE_LINK_STATUS_INTERNAL;
    default:
        return DEVICE_LINK_STATUS_INTERNAL;
    }
}

static void _bridge_update_status(
    const connectivity_manager_status_snapshot_t *snapshot)
{
    /* WifiStatus payloads are bounded by WIFI_STATUS_RESULT_MAX_BYTES;
     * the full operation-result cap (3 KB) would be wasted stack on the
     * connectivity publisher context. */
    uint8_t result[WIFI_STATUS_RESULT_MAX_BYTES];
    size_t result_len = 0U;
    device_link_status_t status = _bridge_map_failure(snapshot->failure);
    device_link_operation_state_t state = DEVICE_LINK_OPERATION_FAILED;

    if (snapshot->last_error == ESP_OK)
    {
        state = DEVICE_LINK_OPERATION_SUCCEEDED;
        status = DEVICE_LINK_STATUS_OK;
        if (device_link_wifi_adapter_encode_operation_result(
                    snapshot, result, sizeof(result), &result_len) != ESP_OK)
        {
            /* A SUCCEEDED record for a result-declaring method must carry
             * the WifiStatus payload. Encoding failed on a malformed
             * snapshot, so fail the record honestly instead of publishing
             * a contract violation. */
            state = DEVICE_LINK_OPERATION_FAILED;
            status = DEVICE_LINK_STATUS_INTERNAL;
            result_len = 0U;
        }
    }
    else if (status == DEVICE_LINK_STATUS_OK)
    {
        /* Terminal failure without a classified failure value: fall back
         * to the diagnostic error. */
        status = _map_result((esp_err_t)snapshot->last_error);
    }
    const esp_err_t update_result = ble_link_service_async_operation_update(
                                        snapshot->operation_id, state, status,
                                        result, result_len);

    if (update_result == ESP_ERR_NOT_FOUND)
    {
        /* The manager terminal arrived before the Core v2 table admission:
         * retain it briefly; async_operation_start merges it into the
         * freshly admitted record. */
        const esp_err_t defer_result =
            ble_link_service_async_operation_defer_update(
                snapshot->operation_id, state, status, result, result_len);

        if (defer_result != ESP_OK)
        {
            LOG_W("operation bridge defer failed result=%d", defer_result);
        }
    }
    else if (update_result != ESP_OK)
    {
        LOG_W("operation bridge update failed result=%d", update_result);
    }
}

static void _bridge_update_scan(
    const connectivity_manager_scan_snapshot_t *snapshot)
{
    /* start_scan declares core.v2.Empty: a SUCCEEDED record carries no
     * result payload; scan pages are read via GetScanResults. */
    device_link_status_t status =
        _map_result((esp_err_t)snapshot->last_error);
    const device_link_operation_state_t state =
        status == DEVICE_LINK_STATUS_OK ?
        DEVICE_LINK_OPERATION_SUCCEEDED : DEVICE_LINK_OPERATION_FAILED;
    const esp_err_t update_result = ble_link_service_async_operation_update(
                                        snapshot->operation_id, state, status,
                                        NULL, 0U);

    if (update_result == ESP_ERR_NOT_FOUND)
    {
        const esp_err_t defer_result =
            ble_link_service_async_operation_defer_update(
                snapshot->operation_id, state, status, NULL, 0U);

        if (defer_result != ESP_OK)
        {
            LOG_W("scan bridge defer failed result=%d", defer_result);
        }
    }
    else if (update_result != ESP_OK)
    {
        LOG_W("scan bridge update failed result=%d", update_result);
    }
}

static void _bridge_event(event_bus_msg_id_t msg_id, uint32_t sub_type,
                          const void *payload, size_t payload_size,
                          void *user_data)
{
    (void)msg_id;
    (void)user_data;
    if (sub_type == CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT)
    {
        const connectivity_manager_status_snapshot_t *snapshot = payload;

        if (payload == NULL ||
                payload_size != sizeof(connectivity_manager_status_snapshot_t) ||
                !snapshot->operation_complete || snapshot->operation_id == 0U)
        {
            return;
        }
        _bridge_update_status(snapshot);
        return;
    }
    if (sub_type == CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT)
    {
        const connectivity_manager_scan_snapshot_t *snapshot = payload;

        if (payload == NULL ||
                payload_size != sizeof(connectivity_manager_scan_snapshot_t) ||
                snapshot->running || snapshot->operation_id == 0U)
        {
            return;
        }
        _bridge_update_scan(snapshot);
    }
}

esp_err_t device_link_wifi_adapter_bridge_start(void)
{
    if (s_bridge_handle != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        return ESP_OK;
    }
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    const esp_err_t result = event_bus_subscribe(
                                 EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                                 EVENT_BUS_SUB_TYPE_ANY, _bridge_event, NULL,
                                 EVENT_BUS_DISPATCH_PUBLISHER, &handle);

    if (result != ESP_OK)
    {
        return result;
    }
    s_bridge_handle = handle;
    return ESP_OK;
}

void device_link_wifi_adapter_bridge_stop(void)
{
    if (s_bridge_handle == EVENT_BUS_SUB_HANDLE_INVALID)
    {
        return;
    }
    const esp_err_t result = event_bus_unsubscribe(s_bridge_handle);

    if (result != ESP_OK)
    {
        /* Retain the handle on failure so a retry can unsubscribe: clearing
         * it would let bridge_start subscribe again and duplicate the live
         * callback for the same connectivity events. */
        LOG_W("bridge unsubscribe failed result=%d", result);
        return;
    }
    s_bridge_handle = EVENT_BUS_SUB_HANDLE_INVALID;
}

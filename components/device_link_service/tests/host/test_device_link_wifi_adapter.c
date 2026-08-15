#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "connectivity_manager.h"
#include "device_link_protocol.h"
#include "device_link_tlv.h"
#include "device_link_wifi_adapter.h"

/* Test-only fake hook (fakes/connectivity_manager.c). */
extern void connectivity_manager_fake_set_scan(
    const connectivity_manager_scan_snapshot_t *snapshot);

static bool contains_byte(const uint8_t *data, size_t len, uint8_t value)
{
    for (size_t i = 0U; i < len; ++i)
    {
        if (data[i] == value)
        {
            return true;
        }
    }
    return false;
}

static void _fill_scan(connectivity_manager_scan_snapshot_t *scan)
{
    memset(scan, 0, sizeof(*scan));
    scan->generation = UINT64_C(0x0102030405060708);
    scan->truncated = true;
    for (size_t i = 0U; i < CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS; ++i)
    {
        connectivity_manager_scan_record_t *record = &scan->records[i];

        snprintf(record->ssid, sizeof(record->ssid), "AP-%zu", i);
        record->rssi = -40 - (int)i;
        record->channel = (uint8_t)(1U + i);
        record->security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL;
        record->saved = i == 0U;
        scan->record_count++;
    }
}

static void _make_scan_query(
    uint64_t generation, uint64_t page, uint8_t *out, size_t *out_len)
{
    device_link_tlv_writer_t writer;

    device_link_tlv_writer_init(&writer, out, 64U);
    assert(device_link_tlv_put_fixed64(&writer, 1U, generation) == ESP_OK);
    assert(device_link_tlv_put_uint(&writer, 2U, page) == ESP_OK);
    assert(device_link_tlv_writer_finish(&writer, out_len) == ESP_OK);
}

static void _count_scan_response(
    const uint8_t *response, size_t response_len,
    uint64_t *out_page, size_t *out_networks, bool *out_has_more,
    bool *out_truncated)
{
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has = false;

    *out_page = 0U;
    *out_networks = 0U;
    *out_has_more = false;
    *out_truncated = false;
    assert(device_link_tlv_reader_init(&reader, response, response_len) ==
           ESP_OK);
    while (device_link_tlv_reader_next(&reader, &field, &has) == ESP_OK &&
            has)
    {
        if (field.id == 2U)
        {
            *out_page = field.value.unsigned_value;
        }
        else if (field.id == 3U)
        {
            (*out_networks)++;
        }
        else if (field.id == 4U)
        {
            *out_has_more = field.value.unsigned_value != 0U;
        }
        else if (field.id == 5U)
        {
            *out_truncated = field.value.unsigned_value != 0U;
        }
    }
    assert(reader.offset == reader.len);
}

static void test_scan_results_are_paged(void)
{
    const device_link_domain_descriptor_t *descriptor = NULL;
    connectivity_manager_scan_snapshot_t scan;
    uint8_t request[64];
    size_t request_len = 0U;
    uint8_t response[768];
    size_t response_len = 0U;
    const device_link_request_context_t context =
    {
        .header =
        {
            .domain_id = DEVICE_LINK_DOMAIN_WIFI,
            .domain_major = 1U,
            .method_id = 3U,
            .call_id = 1U,
            .boot_id = 1U,
        },
        .security_authenticated = true,
        .authorized = true,
    };
    uint64_t page = 0U;
    size_t networks = 0U;
    bool has_more = false;
    bool truncated = false;

    _fill_scan(&scan);
    connectivity_manager_fake_set_scan(&scan);
    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    /* Page 0 carries every record below the page size and no tail. */
    _make_scan_query(scan.generation, 0U, request, &request_len);
    assert(descriptor->methods[2].handler(
               &context, request, request_len, response, sizeof(response),
               &response_len, descriptor->methods[2].handler_arg) ==
           DEVICE_LINK_STATUS_OK);
    _count_scan_response(response, response_len, &page, &networks,
                         &has_more, &truncated);
    assert(page == 0U);
    assert(networks == scan.record_count);
    assert(!has_more);
    assert(truncated);
    /* A page past the snapshot end is empty, not an error. */
    _make_scan_query(scan.generation, 7U, request, &request_len);
    assert(descriptor->methods[2].handler(
               &context, request, request_len, response, sizeof(response),
               &response_len, descriptor->methods[2].handler_arg) ==
           DEVICE_LINK_STATUS_OK);
    _count_scan_response(response, response_len, &page, &networks,
                         &has_more, &truncated);
    assert(page == 7U);
    assert(networks == 0U);
    assert(!has_more);
    /* A stale generation is NOT_FOUND. */
    _make_scan_query(scan.generation + 1U, 0U, request, &request_len);
    assert(descriptor->methods[2].handler(
               &context, request, request_len, response, sizeof(response),
               &response_len, descriptor->methods[2].handler_arg) ==
           DEVICE_LINK_STATUS_NOT_FOUND);
    connectivity_manager_fake_set_scan(NULL);
}

static void test_descriptor_is_static_and_complete(void)
{
    const device_link_domain_descriptor_t *descriptor = NULL;

    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    assert(descriptor != NULL);
    assert(descriptor->domain_id == DEVICE_LINK_DOMAIN_WIFI);
    assert(descriptor->major == 1U);
    assert(descriptor->method_count == 8U);
    for (size_t i = 0U; i < descriptor->method_count; ++i)
    {
        assert(descriptor->methods[i].method_id == i + 1U);
        assert(descriptor->methods[i].handler != NULL);
        assert(descriptor->methods[i].request_schema != NULL);
        assert(descriptor->methods[i].response_schema != NULL);
    }
}

static void test_credentials_are_delegated_without_echo(void)
{
    const device_link_domain_descriptor_t *descriptor = NULL;
    uint8_t nested[32];
    uint8_t request[64];
    device_link_tlv_writer_t nested_writer;
    device_link_tlv_writer_t writer;
    size_t nested_len = 0U;
    size_t request_len = 0U;

    device_link_tlv_writer_init(&nested_writer, nested, sizeof(nested));
    assert(device_link_tlv_put_bytes(&nested_writer, 1U,
                                     (const uint8_t *)"AP1", 3U) == ESP_OK);
    assert(device_link_tlv_put_bytes(&nested_writer, 2U,
                                     (const uint8_t *)"p", 1U) == ESP_OK);
    assert(device_link_tlv_put_uint(&nested_writer, 3U, 1U) == ESP_OK);
    assert(device_link_tlv_writer_finish(&nested_writer, &nested_len) ==
           ESP_OK);
    device_link_tlv_writer_init(&writer, request, sizeof(request));
    assert(device_link_tlv_put_bytes(&writer, 1U, nested, nested_len) ==
           ESP_OK);
    assert(device_link_tlv_put_fixed64(&writer, 2U,
                                       UINT64_C(0x0102030405060708)) == ESP_OK);
    assert(device_link_tlv_put_bool(&writer, 3U, true) == ESP_OK);
    assert(device_link_tlv_writer_finish(&writer, &request_len) == ESP_OK);
    uint8_t response[128];
    size_t response_len = 0U;
    const device_link_request_context_t context =
    {
        .header =
        {
            .domain_id = DEVICE_LINK_DOMAIN_WIFI,
            .domain_major = 1U,
            .method_id = 4U,
            .call_id = 1U,
            .boot_id = 1U,
        },
        .security_authenticated = true,
        .authorized = true,
    };

    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    assert(descriptor->methods[3].handler(&context, request, request_len,
                                          response, sizeof(response),
                                          &response_len,
                                          descriptor->methods[3].handler_arg) ==
           DEVICE_LINK_STATUS_OK);
    assert(response_len != 0U);
    assert(!contains_byte(response, response_len, (uint8_t)'p'));
}

static void test_unauthorized_calls_are_rejected(void)
{
    const device_link_domain_descriptor_t *descriptor = NULL;
    const device_link_request_context_t context =
    {
        .header =
        {
            .domain_id = DEVICE_LINK_DOMAIN_WIFI,
            .domain_major = 1U,
            .method_id = 1U,
            .call_id = 1U,
            .boot_id = 1U,
        },
        .security_authenticated = true,
        .authorized = false,
    };
    uint8_t response[128];
    size_t response_len = 0U;

    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    assert(descriptor->methods[0].handler(&context, NULL, 0U, response,
                                          sizeof(response), &response_len,
                                          descriptor->methods[0].handler_arg) ==
           DEVICE_LINK_STATUS_PERMISSION_DENIED);
}

int main(void)
{
    test_descriptor_is_static_and_complete();
    test_credentials_are_delegated_without_echo();
    test_unauthorized_calls_are_rejected();
    test_scan_results_are_paged();
    return 0;
}

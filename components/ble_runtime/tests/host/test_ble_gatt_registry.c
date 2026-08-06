#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_gatt_registry.h"

#define TEST_ASSERT_TRUE(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "assertion failed at line %d: %s\n", \
                    __LINE__, #condition); \
            abort(); \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL(expected, actual) \
    do \
    { \
        const long expected_value = (long)(expected); \
        const long actual_value = (long)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%ld != %ld)\n", \
                    __LINE__, #expected, #actual, expected_value, actual_value); \
            abort(); \
        } \
    } while (0)

static int _dummy_access(uint16_t conn_handle, uint16_t attr_handle,
                         ble_gatt_registry_access_context_t *context, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)context;
    (void)arg;
    return 0;
}

static const uint8_t s_service_uuid[16] =
{
    0xa3, 0x4e, 0x85, 0x57, 0x11, 0x3d, 0x8a, 0xa2,
    0x59, 0x4e, 0xbb, 0xb4, 0x92, 0x31, 0x20, 0x3e,
};
static const uint8_t s_transfer_uuid[16] =
{
    0x3a, 0x88, 0x03, 0x4c, 0xf6, 0xb8, 0x62, 0xb5,
    0x9c, 0x4a, 0x40, 0x1e, 0xc7, 0x5a, 0x73, 0x11,
};
static const uint8_t s_link_state_uuid[16] =
{
    0xa2, 0xf0, 0xcd, 0xfc, 0xe0, 0xe6, 0x5c, 0xb8,
    0xd8, 0x4d, 0x4c, 0xcb, 0x43, 0xe6, 0x01, 0x48,
};
static const uint8_t s_session_rx_uuid[16] =
{
    0x05, 0x2a, 0xaf, 0xd2, 0x5f, 0xec, 0xa1, 0x83,
    0x2c, 0x40, 0xac, 0xbe, 0x10, 0x57, 0xe8, 0x2b,
};
static const uint8_t s_unknown_uuid[16] =
{
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static const ble_gatt_registry_characteristic_t s_link_chrs[] =
{
    {
        .uuid = s_link_state_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_READ |
        BLE_GATT_REGISTRY_PROP_NOTIFY,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
        .access_cb = _dummy_access,
    },
    {
        .uuid = s_session_rx_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_WRITE,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
        .access_cb = _dummy_access,
    },
};

static const ble_gatt_registry_service_t s_link_service =
{
    .uuid = s_service_uuid,
    .characteristics = s_link_chrs,
    .characteristic_count = 2U,
};

static void test_register_lookup_and_seal(void)
{
    const ble_gatt_registry_characteristic_t *found = NULL;

    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_seal());
    TEST_ASSERT_TRUE(ble_gatt_registry_is_sealed());
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_lookup_by_uuid(s_link_state_uuid,
                              &found));
    TEST_ASSERT_TRUE(found == &s_link_chrs[0]);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_lookup_by_uuid(s_unknown_uuid,
                              &found));
}

static void test_handle_assignment_after_seal(void)
{
    const ble_gatt_registry_characteristic_t *found = NULL;
    uint16_t assigned = 0U;

    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_seal());
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_assign_handle(s_link_state_uuid, 5U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_assign_handle(s_session_rx_uuid, 7U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_lookup_by_handle(7U, &found));
    TEST_ASSERT_TRUE(found == &s_link_chrs[1]);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_lookup_by_handle(9U, &found));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_get_assigned_handle(s_link_state_uuid,
                              &assigned));
    TEST_ASSERT_EQUAL(5U, assigned);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gatt_registry_lookup_by_handle(0U, &found));
}

static void test_handle_reassignment_and_idempotence(void)
{
    uint16_t assigned = 0U;

    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_assign_handle(s_link_state_uuid, 5U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_assign_handle(s_link_state_uuid, 5U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_assign_handle(s_link_state_uuid, 9U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_get_assigned_handle(s_link_state_uuid,
                              &assigned));
    TEST_ASSERT_EQUAL(9U, assigned);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_gatt_registry_assign_handle(s_session_rx_uuid, 9U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gatt_registry_assign_handle(s_session_rx_uuid, 0U));
}

static void test_duplicate_registration_rejected_atomically(void)
{
    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_gatt_registry_register(&s_link_service));

    static const ble_gatt_registry_characteristic_t duplicate_chrs[] =
    {
        {
            .uuid = s_link_state_uuid,
            .properties = BLE_GATT_REGISTRY_PROP_READ,
            .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
            .access_cb = _dummy_access,
        },
    };
    static const ble_gatt_registry_service_t duplicate_service =
    {
        .uuid = s_session_rx_uuid,
        .characteristics = duplicate_chrs,
        .characteristic_count = 1U,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_gatt_registry_register(&duplicate_service));
    TEST_ASSERT_EQUAL(2U, s_link_service.characteristic_count);
}

static void test_partial_registration_rolls_back(void)
{
    static const uint8_t bad_chr_uuid[16] =
    {
        0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
        0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
    };
    static const ble_gatt_registry_characteristic_t bad_chrs[] =
    {
        {
            .uuid = s_unknown_uuid,
            .properties = BLE_GATT_REGISTRY_PROP_READ,
            .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
            .access_cb = _dummy_access,
        },
        {
            .uuid = bad_chr_uuid,
            .properties = BLE_GATT_REGISTRY_PROP_READ,
            .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
            .access_cb = NULL,
        },
    };
    static const ble_gatt_registry_service_t bad_service =
    {
        .uuid = s_transfer_uuid,
        .characteristics = bad_chrs,
        .characteristic_count = 2U,
    };
    const ble_gatt_registry_characteristic_t *found_chr = NULL;
    const ble_gatt_registry_service_t *found_svc = NULL;

    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gatt_registry_register(&bad_service));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_lookup_by_uuid(s_unknown_uuid,
                              &found_chr));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_get_service(1U, &found_svc));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_get_characteristic(1U,
                      &found_chr));
    TEST_ASSERT_TRUE(found_chr == &s_link_chrs[1]);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_get_characteristic(2U, &found_chr));
}

static void test_intra_service_duplicate_rejected(void)
{
    static const ble_gatt_registry_characteristic_t dup_chrs[] =
    {
        {
            .uuid = s_unknown_uuid,
            .properties = BLE_GATT_REGISTRY_PROP_READ,
            .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
            .access_cb = _dummy_access,
        },
        {
            .uuid = s_unknown_uuid,
            .properties = BLE_GATT_REGISTRY_PROP_READ,
            .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
            .access_cb = _dummy_access,
        },
    };
    static const ble_gatt_registry_service_t dup_service =
    {
        .uuid = s_transfer_uuid,
        .characteristics = dup_chrs,
        .characteristic_count = 2U,
    };
    const ble_gatt_registry_service_t *found_svc = NULL;
    const ble_gatt_registry_characteristic_t *found_chr = NULL;

    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_gatt_registry_register(&dup_service));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_get_service(0U, &found_svc));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_get_characteristic(0U, &found_chr));
}

static void test_oversized_count_rejected(void)
{
    static const ble_gatt_registry_characteristic_t single_chr =
    {
        .uuid = s_unknown_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_READ,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
        .access_cb = _dummy_access,
    };
    static const ble_gatt_registry_service_t huge_service =
    {
        .uuid = s_transfer_uuid,
        .characteristics = &single_chr,
        .characteristic_count = (size_t) -1,
    };
    const ble_gatt_registry_service_t *found_svc = NULL;

    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      ble_gatt_registry_register(&huge_service));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      ble_gatt_registry_register(&huge_service));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_get_service(0U, &found_svc));
    TEST_ASSERT_TRUE(found_svc == &s_link_service);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_get_service(1U, &found_svc));
}

static void test_seal_rejects_registration_only(void)
{
    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_seal());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_gatt_registry_seal());
}

static void test_invalid_arguments_rejected(void)
{
    const ble_gatt_registry_characteristic_t *found = NULL;
    uint16_t assigned = 0U;

    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_gatt_registry_register(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gatt_registry_lookup_by_handle(1U, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gatt_registry_lookup_by_uuid(s_link_state_uuid,
                              NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gatt_registry_get_assigned_handle(s_link_state_uuid,
                              NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gatt_registry_assign_handle(NULL, 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_lookup_by_handle(1U, &found));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_lookup_by_uuid(s_unknown_uuid,
                              &found));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_get_assigned_handle(s_unknown_uuid,
                              &assigned));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_assign_handle(s_unknown_uuid, 3U));
}

static void test_iteration_ordering(void)
{
    const ble_gatt_registry_characteristic_t *found_chr = NULL;
    const ble_gatt_registry_service_t *found_svc = NULL;

    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_get_characteristic(0U,
                      &found_chr));
    TEST_ASSERT_TRUE(found_chr == &s_link_chrs[0]);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_get_characteristic(1U,
                      &found_chr));
    TEST_ASSERT_TRUE(found_chr == &s_link_chrs[1]);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_get_characteristic(2U, &found_chr));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_get_service(0U, &found_svc));
    TEST_ASSERT_TRUE(found_svc == &s_link_service);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_get_service(1U, &found_svc));
}

static void test_restart_clear_handles_reassigns(void)
{
    uint16_t assigned = 0U;

    ble_gatt_registry_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_register(&s_link_service));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_assign_handle(s_link_state_uuid, 5U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_assign_handle(s_session_rx_uuid, 7U));
    ble_gatt_registry_clear_handles();
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_assign_handle(s_link_state_uuid, 7U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_assign_handle(s_session_rx_uuid, 5U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_get_assigned_handle(s_link_state_uuid,
                              &assigned));
    TEST_ASSERT_EQUAL(7U, assigned);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_gatt_registry_get_assigned_handle(s_session_rx_uuid,
                              &assigned));
    TEST_ASSERT_EQUAL(5U, assigned);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_gatt_registry_get_assigned_handle(s_unknown_uuid,
                              &assigned));
}

int main(void)
{
    test_register_lookup_and_seal();
    test_handle_assignment_after_seal();
    test_handle_reassignment_and_idempotence();
    test_restart_clear_handles_reassigns();
    test_duplicate_registration_rejected_atomically();
    test_partial_registration_rolls_back();
    test_intra_service_duplicate_rejected();
    test_oversized_count_rejected();
    test_seal_rejects_registration_only();
    test_invalid_arguments_rejected();
    test_iteration_ordering();
    printf("ble_gatt_registry: all tests passed\n");
    return 0;
}

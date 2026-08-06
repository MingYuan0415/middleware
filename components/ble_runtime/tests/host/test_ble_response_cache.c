#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_response_cache.h"

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

#define TEST_ASSERT_FALSE(condition) \
    do \
    { \
        if ((condition)) \
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

static uint32_t s_now_ms;

static uint32_t _fake_now_ms(void)
{
    return s_now_ms;
}

static void _init_cache(void)
{
    static const ble_response_cache_config_t config =
    {
        .max_entries = 4U,
        .max_entry_bytes = 64U,
        .max_key_bytes = 8U,
        .ttl_ms = 1000U,
        .now_ms = _fake_now_ms,
        .lock = NULL,
        .unlock = NULL,
        .lock_arg = NULL,
    };

    ble_response_cache_deinit();
    ble_used_id_set_deinit();
    s_now_ms = 5000U;
    ble_response_cache_init(&config);
    ble_used_id_set_init(&config);
}

static void test_put_get_roundtrip(void)
{
    const uint8_t key[2] = {0x12, 0x34};
    const uint8_t payload[5] = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t out[16];
    size_t out_len = 0U;

    _init_cache();
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                          3U, key, sizeof(key), payload,
                          sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_get(
                          3U, key, sizeof(key), out, sizeof(out),
                          &out_len));
    TEST_ASSERT_EQUAL(5U, out_len);
    TEST_ASSERT_EQUAL(0, memcmp(out, payload, sizeof(payload)));
}

static void test_put_replaces_same_key(void)
{
    const uint8_t key[2] = {0x12, 0x34};
    const uint8_t payload[3] = {0xaa, 0xbb, 0xcc};
    uint8_t out[16];
    size_t out_len = 0U;

    _init_cache();
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                          3U, key, sizeof(key),
                          (const uint8_t[])
    {
        0x01
    }, 1U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                          3U, key, sizeof(key), payload,
                          sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_get(
                          3U, key, sizeof(key), out, sizeof(out),
                          &out_len));
    TEST_ASSERT_EQUAL(3U, out_len);
    TEST_ASSERT_EQUAL(0, memcmp(out, payload, sizeof(payload)));
}

static void test_generation_isolated(void)
{
    const uint8_t key[2] = {0x12, 0x34};
    const uint8_t payload[2] = {0x01, 0x02};
    uint8_t out[16];
    size_t out_len = 0U;

    _init_cache();
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                          1U, key, sizeof(key), payload,
                          sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_response_cache_get(
                          2U, key, sizeof(key), out,
                          sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_get(
                          1U, key, sizeof(key), out, sizeof(out),
                          &out_len));
}

static void test_ttl_expiry(void)
{
    const uint8_t key[2] = {0x12, 0x34};
    const uint8_t payload[2] = {0x01, 0x02};
    uint8_t out[16];
    size_t out_len = 0U;

    _init_cache();
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                          3U, key, sizeof(key), payload,
                          sizeof(payload)));
    s_now_ms += 1001U;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_response_cache_get(
                          3U, key, sizeof(key), out,
                          sizeof(out), &out_len));
}

static void test_eviction_when_full(void)
{
    const uint8_t payload[2] = {0x01, 0x02};
    uint8_t out[16];
    size_t out_len = 0U;

    _init_cache();
    for (uint8_t i = 0U; i < 4U; ++i)
    {
        const uint8_t key[1] = {i};

        TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                              3U, key, 1U, payload,
                              sizeof(payload)));
    }
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                          3U, (const uint8_t[])
    {
        9U
    }, 1U, payload,
    sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_response_cache_get(
                          3U, (const uint8_t[])
    {
        0U
    }, 1U,
    out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_get(
                          3U, (const uint8_t[])
    {
        9U
    }, 1U, out,
    sizeof(out), &out_len));
}

static void test_clear_generation_and_clear(void)
{
    const uint8_t payload[2] = {0x01, 0x02};
    uint8_t out[16];
    size_t out_len = 0U;

    _init_cache();
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                          1U, (const uint8_t[])
    {
        0x11
    }, 1U, payload,
    sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                          2U, (const uint8_t[])
    {
        0x22
    }, 1U, payload,
    sizeof(payload)));
    ble_response_cache_clear_generation(1U);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_response_cache_get(
                          1U, (const uint8_t[])
    {
        0x11
    }, 1U,
    out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_get(
                          2U, (const uint8_t[])
    {
        0x22
    }, 1U, out,
    sizeof(out), &out_len));
    ble_response_cache_clear();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_response_cache_get(
                          2U, (const uint8_t[])
    {
        0x22
    }, 1U,
    out, sizeof(out), &out_len));
}

static void test_invalid_arguments_rejected(void)
{
    uint8_t out[16];
    size_t out_len = 0U;

    _init_cache();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_response_cache_put(
                          1U, NULL, 1U,
                          (const uint8_t[])
    {
        0x01
    }, 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_response_cache_put(
                          1U, (const uint8_t[])
    {
        0x11
    },
    1U, NULL, 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_response_cache_put(
                          1U, (const uint8_t[])
    {
        0x11
    },
    9U, (const uint8_t[])
    {
        0x01
    },
    1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_response_cache_put(
                          1U, (const uint8_t[])
    {
        0x11
    },
    1U, (const uint8_t[])
    {
        0x01
    },
    65U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_response_cache_get(
                          1U, (const uint8_t[])
    {
        0x11
    },
    1U, out, 0U, &out_len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_response_cache_get(
                          1U, (const uint8_t[])
    {
        0x11
    },
    1U, NULL, 16U, &out_len));
    ble_response_cache_deinit();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_response_cache_put(
                          1U, (const uint8_t[])
    {
        0x11
    },
    1U,
    (const uint8_t[])
    {
        0x01
    },
    1U));
}

static void test_used_ids_independent_from_cache(void)
{
    const uint8_t key[2] = {0x12, 0x34};
    const uint8_t payload[2] = {0x01, 0x02};
    uint8_t out[16];
    size_t out_len = 0U;

    _init_cache();
    TEST_ASSERT_FALSE(ble_used_id_set_contains(1U, 42U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_used_id_set_add(1U, 42U));
    TEST_ASSERT_TRUE(ble_used_id_set_contains(1U, 42U));
    TEST_ASSERT_FALSE(ble_used_id_set_contains(1U, 43U));
    TEST_ASSERT_FALSE(ble_used_id_set_contains(2U, 42U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_put(
                          1U, key, sizeof(key), payload,
                          sizeof(payload)));
    ble_used_id_set_clear_generation(1U);
    TEST_ASSERT_FALSE(ble_used_id_set_contains(1U, 42U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_response_cache_get(
                          1U, key, sizeof(key), out, sizeof(out),
                          &out_len));
    TEST_ASSERT_EQUAL(2U, out_len);
    ble_response_cache_clear();
    TEST_ASSERT_FALSE(ble_used_id_set_contains(1U, 42U));
}

static void test_used_ids_fifo_eviction(void)
{
    _init_cache();
    for (uint32_t i = 0U; i < 6U; ++i)
    {
        TEST_ASSERT_EQUAL(ESP_OK, ble_used_id_set_add(1U, i));
    }
    TEST_ASSERT_FALSE(ble_used_id_set_contains(1U, 0U));
    TEST_ASSERT_FALSE(ble_used_id_set_contains(1U, 1U));
    TEST_ASSERT_TRUE(ble_used_id_set_contains(1U, 4U));
    TEST_ASSERT_TRUE(ble_used_id_set_contains(1U, 5U));
    ble_used_id_set_clear();
    TEST_ASSERT_FALSE(ble_used_id_set_contains(1U, 5U));
    ble_used_id_set_deinit();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_used_id_set_add(1U, 7U));
    TEST_ASSERT_FALSE(ble_used_id_set_contains(1U, 7U));
}

int main(void)
{
    test_put_get_roundtrip();
    test_put_replaces_same_key();
    test_generation_isolated();
    test_ttl_expiry();
    test_eviction_when_full();
    test_clear_generation_and_clear();
    test_invalid_arguments_rejected();
    test_used_ids_independent_from_cache();
    test_used_ids_fifo_eviction();
    ble_response_cache_deinit();
    ble_used_id_set_deinit();
    printf("ble_response_cache: all tests passed\n");
    return 0;
}

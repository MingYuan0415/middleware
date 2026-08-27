#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_gatt_registry.h"
#include "ble_link_gatt.h"
#include "ble_link_session.h"
#include "ble_tx_scheduler.h"
#include "host_freertos.h"

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
        const long long expected_value = (long long)(expected); \
        const long long actual_value = (long long)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%lld != %lld)\n", \
                    __LINE__, #expected, #actual, expected_value, \
                    actual_value); \
            abort(); \
        } \
    } while (0)

#define BOOT_ID 72623859790382856ULL
#define GEN 1U

static esp_err_t _fake_notify(uint16_t conn_handle, uint16_t value_handle,
                              const uint8_t *data, size_t len)
{
    (void)conn_handle;
    (void)value_handle;
    (void)data;
    (void)len;
    return ESP_OK;
}

static esp_err_t _fake_indicate(uint16_t conn_handle, uint16_t value_handle,
                                const uint8_t *data, size_t len)
{
    (void)conn_handle;
    (void)value_handle;
    (void)data;
    (void)len;
    return ESP_OK;
}

static const ble_port_ops_t s_fake_port_ops =
{
    .adv_start = NULL,
    .adv_stop = NULL,
    .notify = _fake_notify,
    .indicate = _fake_indicate,
};

static const ble_tx_scheduler_config_t s_scheduler_config =
{
    .queue_depth = 32U,
    .max_frame_bytes = 512U,
    .ops = &s_fake_port_ops,
};

static ble_link_gatt_config_t s_config;

static void _reset(void)
{
    ble_link_gatt_reset();
    ble_tx_scheduler_deinit();
    memset(&s_config, 0, sizeof(s_config));
    s_config.boot_id = BOOT_ID;
    s_config.connection_generation = GEN;
    s_config.conn_handle = 7U;
    s_config.att_mtu = 23U;
    s_config.tx_queue_depth = 32U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_init(&s_scheduler_config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_init(&s_config));
}

static void test_registers_two_characteristics(void)
{
    static const uint8_t command_rx[16] =
    {
        0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
        0x44, 0x4f, 0xf6, 0x65, 0x11, 0x8c, 0x2a, 0x8f,
    };
    static const uint8_t server_tx[16] =
    {
        0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
        0x44, 0x4f, 0xf6, 0x65, 0x12, 0x8c, 0x2a, 0x8f,
    };

    _reset();
    {
        const esp_err_t sealed = ble_gatt_registry_seal();

        TEST_ASSERT_TRUE(sealed == ESP_OK ||
                         sealed == ESP_ERR_INVALID_STATE);
    }
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          command_rx, 0x10U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          server_tx, 0x12U));
    ble_link_gatt_update_handles();
    TEST_ASSERT_EQUAL(0x10U, ble_link_gatt_session_rx_handle());
    TEST_ASSERT_EQUAL(0x12U, ble_link_gatt_session_tx_handle());
    TEST_ASSERT_EQUAL(0U, ble_link_gatt_link_state_handle());
    TEST_ASSERT_EQUAL(0U, ble_link_gatt_control_tx_handle());
}

static void test_att_mtu_clamps_to_profile(void)
{
    uint32_t mtu = 0U;

    _reset();
    ble_link_gatt_set_att_mtu(23U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
    TEST_ASSERT_EQUAL(23U, mtu);
    ble_link_gatt_set_att_mtu(1024U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
    TEST_ASSERT_EQUAL(BLE_LINK_GATT_ATT_MTU_MAX, mtu);
}

static void test_connection_updates_generation(void)
{
    const uint8_t addr[6] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U };

    _reset();
    ble_link_session_init(BOOT_ID);
    ble_link_gatt_set_connection(2U, 9U, 1U, addr);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_update_identity(
                          2U, 9U, 1U, addr));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_gatt_update_identity(
                          1U, 9U, 1U, addr));
}

int main(void)
{
    test_registers_two_characteristics();
    test_att_mtu_clamps_to_profile();
    test_connection_updates_generation();
    return 0;
}

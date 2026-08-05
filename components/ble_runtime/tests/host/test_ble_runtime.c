#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_runtime.h"

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

static unsigned int s_init_calls;
static unsigned int s_start_calls;
static unsigned int s_stop_calls;
static unsigned int s_deinit_calls;
static esp_err_t s_init_result;
static esp_err_t s_start_result;
static esp_err_t s_stop_result;
static esp_err_t s_deinit_result;

static esp_err_t _port_init(void)
{
    ++s_init_calls;
    return s_init_result;
}

static esp_err_t _port_start(void)
{
    ++s_start_calls;
    return s_start_result;
}

static esp_err_t _port_stop(void)
{
    ++s_stop_calls;
    return s_stop_result;
}

static esp_err_t _port_deinit(void)
{
    ++s_deinit_calls;
    return s_deinit_result;
}

static const ble_runtime_host_port_t s_ok_port =
{
    .init = _port_init,
    .start = _port_start,
    .stop = _port_stop,
    .deinit = _port_deinit,
};

static void _reset_port(void)
{
    s_init_calls = 0U;
    s_start_calls = 0U;
    s_stop_calls = 0U;
    s_deinit_calls = 0U;
    s_init_result = ESP_OK;
    s_start_result = ESP_OK;
    s_stop_result = ESP_OK;
    s_deinit_result = ESP_OK;
}

static void test_init_rejects_bad_config(void)
{
    _reset_port();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_runtime_init(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_runtime_init(&(ble_runtime_config_t)
    {
        .port = NULL
    }));
    const ble_runtime_host_port_t missing_init =
    {
        .start = _port_start,
        .stop = _port_stop,
        .deinit = _port_deinit,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_runtime_init(&(ble_runtime_config_t)
    {
        .port = &missing_init
    }));
    const ble_runtime_host_port_t missing_deinit =
    {
        .init = _port_init,
        .start = _port_start,
        .stop = _port_stop,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_runtime_init(&(ble_runtime_config_t)
    {
        .port = &missing_deinit
    }));
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_STOPPED, ble_runtime_get_state());
}

static void test_happy_path_lifecycle(void)
{
    _reset_port();
    const ble_runtime_config_t config = {.port = &s_ok_port};

    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_STOPPED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_start());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_RUNNING, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(1U, s_init_calls);
    TEST_ASSERT_EQUAL(1U, s_start_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_stop());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_STOPPED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    TEST_ASSERT_EQUAL(1U, s_deinit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_deinit());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_STOPPED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(1U, s_deinit_calls);
}

static void test_restart_runs_full_teardown(void)
{
    _reset_port();
    const ble_runtime_config_t config = {.port = &s_ok_port};

    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_start());
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_stop());
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    TEST_ASSERT_EQUAL(1U, s_deinit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_start());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_RUNNING, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(2U, s_init_calls);
    TEST_ASSERT_EQUAL(2U, s_start_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_stop());
    TEST_ASSERT_EQUAL(2U, s_stop_calls);
    TEST_ASSERT_EQUAL(2U, s_deinit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_deinit());
}

static void test_start_init_failure_faults_without_teardown(void)
{
    _reset_port();
    s_init_result = ESP_ERR_NO_MEM;
    const ble_runtime_config_t config = {.port = &s_ok_port};

    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_runtime_start());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_FAULTED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(0U, s_start_calls);
    TEST_ASSERT_EQUAL(0U, s_stop_calls);
    TEST_ASSERT_EQUAL(0U, s_deinit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_stop());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_STOPPED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(0U, s_stop_calls);
    TEST_ASSERT_EQUAL(0U, s_deinit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_deinit());
}

static void test_start_failure_returns_first_error_and_rolls_back(void)
{
    _reset_port();
    s_start_result = ESP_FAIL;
    s_deinit_result = ESP_ERR_NO_MEM;
    const ble_runtime_config_t config = {.port = &s_ok_port};

    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(ESP_FAIL, ble_runtime_start());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_FAULTED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(1U, s_init_calls);
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    TEST_ASSERT_EQUAL(1U, s_deinit_calls);
    s_stop_result = ESP_ERR_INVALID_STATE;
    s_deinit_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_stop());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_STOPPED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    TEST_ASSERT_EQUAL(2U, s_deinit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_deinit());
}

static void test_stop_failure_faults_and_retries(void)
{
    _reset_port();
    s_stop_result = ESP_FAIL;
    const ble_runtime_config_t config = {.port = &s_ok_port};

    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_start());
    TEST_ASSERT_EQUAL(ESP_FAIL, ble_runtime_stop());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_FAULTED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    TEST_ASSERT_EQUAL(0U, s_deinit_calls);
    s_stop_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_stop());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_STOPPED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(2U, s_stop_calls);
    TEST_ASSERT_EQUAL(1U, s_deinit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_deinit());
}

static void test_stop_deinit_failure_faults_and_retries(void)
{
    _reset_port();
    s_deinit_result = ESP_FAIL;
    const ble_runtime_config_t config = {.port = &s_ok_port};

    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_start());
    TEST_ASSERT_EQUAL(ESP_FAIL, ble_runtime_stop());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_FAULTED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    TEST_ASSERT_EQUAL(1U, s_deinit_calls);
    s_stop_result = ESP_ERR_INVALID_STATE;
    s_deinit_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_deinit());
    TEST_ASSERT_EQUAL(BLE_RUNTIME_STATE_STOPPED, ble_runtime_get_state());
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    TEST_ASSERT_EQUAL(2U, s_deinit_calls);
}

static void test_deinit_from_stopped_never_calls_port(void)
{
    _reset_port();
    const ble_runtime_config_t config = {.port = &s_ok_port};

    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_deinit());
    TEST_ASSERT_EQUAL(0U, s_init_calls);
    TEST_ASSERT_EQUAL(0U, s_stop_calls);
    TEST_ASSERT_EQUAL(0U, s_deinit_calls);
}

static void test_repeated_and_invalid_calls_rejected(void)
{
    _reset_port();
    const ble_runtime_config_t config = {.port = &s_ok_port};

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_start());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_stop());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_deinit());
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_start());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_start());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_deinit());
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_stop());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_stop());
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_deinit());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_deinit());
}

static void test_rejected_calls_do_not_touch_port(void)
{
    _reset_port();
    const ble_runtime_config_t config = {.port = &s_ok_port};

    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_start());
    TEST_ASSERT_EQUAL(1U, s_init_calls);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_start());
    TEST_ASSERT_EQUAL(1U, s_init_calls);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_deinit());
    TEST_ASSERT_EQUAL(0U, s_deinit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_stop());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_stop());
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_runtime_deinit());
    TEST_ASSERT_EQUAL(1U, s_deinit_calls);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_runtime_deinit());
    TEST_ASSERT_EQUAL(1U, s_deinit_calls);
}

int main(void)
{
    test_init_rejects_bad_config();
    test_happy_path_lifecycle();
    test_restart_runs_full_teardown();
    test_start_init_failure_faults_without_teardown();
    test_start_failure_returns_first_error_and_rolls_back();
    test_stop_failure_faults_and_retries();
    test_stop_deinit_failure_faults_and_retries();
    test_deinit_from_stopped_never_calls_port();
    test_repeated_and_invalid_calls_rejected();
    test_rejected_calls_do_not_touch_port();
    printf("ble_runtime: all tests passed\n");
    return 0;
}

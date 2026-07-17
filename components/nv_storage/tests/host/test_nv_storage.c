#include "host_freertos.h"
#include "host_nvs.h"
#include "nv_storage.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEST_MAGIC UINT32_C(0x4d545632)

static int s_default_calls;
static int s_second_default_calls;
static int s_validate_calls;
static uint32_t s_reentrant_target;
static esp_err_t s_reentrant_register_result;
static esp_err_t s_reentrant_load_result;
static esp_err_t s_reentrant_init_result;
static esp_err_t s_reentrant_deinit_result;
static esp_err_t s_reentrant_scalar_result;

static pthread_mutex_t s_callback_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_callback_changed = PTHREAD_COND_INITIALIZER;
static bool s_blocking_default_entered;
static bool s_blocking_default_release;

static void _sleep_ms(uint32_t milliseconds)
{
    const struct timespec delay =
    {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static esp_err_t _default_magic(void *data, size_t size)
{
    assert(data != NULL);
    assert(size == sizeof(uint32_t));
    s_default_calls++;
    uint32_t value = TEST_MAGIC;
    memcpy(data, &value, sizeof(value));
    return ESP_OK;
}

static esp_err_t _default_second(void *data, size_t size)
{
    s_second_default_calls++;
    return _default_magic(data, size);
}

static esp_err_t _default_failure(void *data, size_t size)
{
    memset(data, 0xa5, size);
    s_default_calls++;
    return ESP_FAIL;
}

static esp_err_t _default_blocking(void *data, size_t size)
{
    (void)pthread_mutex_lock(&s_callback_lock);
    s_blocking_default_entered = true;
    (void)pthread_cond_broadcast(&s_callback_changed);
    while (!s_blocking_default_release)
    {
        (void)pthread_cond_wait(&s_callback_changed, &s_callback_lock);
    }
    (void)pthread_mutex_unlock(&s_callback_lock);
    return _default_magic(data, size);
}

static bool _validate_magic(const void *data, size_t size)
{
    s_validate_calls++;
    uint32_t value = 0;
    bool valid = data != NULL && size == sizeof(value);
    if (valid)
    {
        memcpy(&value, data, sizeof(value));
        valid = value == TEST_MAGIC;
    }
    return valid;
}

static bool _validate_never(const void *data, size_t size)
{
    (void)data;
    (void)size;
    return false;
}

static esp_err_t _default_reentrant(void *data, size_t size)
{
    s_reentrant_register_result = nv_storage_blob_register(
                                      "nested", &s_reentrant_target,
                                      sizeof(s_reentrant_target), _default_magic,
                                      _validate_magic);
    s_reentrant_load_result = nv_storage_blob_load_all();
    s_reentrant_init_result = nv_storage_init();
    s_reentrant_deinit_result = nv_storage_deinit();
    s_reentrant_scalar_result = nv_storage_set_u8("callback-scalar", 9);
    return _default_magic(data, size);
}

static void _reset_callback_state(void)
{
    s_default_calls = 0;
    s_second_default_calls = 0;
    s_validate_calls = 0;
    s_reentrant_target = 0;
    s_reentrant_register_result = ESP_FAIL;
    s_reentrant_load_result = ESP_FAIL;
    s_reentrant_init_result = ESP_FAIL;
    s_reentrant_deinit_result = ESP_FAIL;
    s_reentrant_scalar_result = ESP_FAIL;
    (void)pthread_mutex_lock(&s_callback_lock);
    s_blocking_default_entered = false;
    s_blocking_default_release = false;
    (void)pthread_mutex_unlock(&s_callback_lock);
}

static void _reset_uninitialized(void)
{
    assert(nv_storage_deinit() == ESP_OK);
    host_nvs_reset();
    _reset_callback_state();
}

static void _reset_initialized(void)
{
    _reset_uninitialized();
    assert(nv_storage_init() == ESP_OK);
}

static void _test_init_policy_and_uninitialized_rejection(void)
{
    _reset_uninitialized();
    uint8_t byte = 0;
    uint32_t blob = 0;
    size_t size = sizeof(blob);
    assert(nv_storage_set_u8("value", 1) == ESP_ERR_INVALID_STATE);
    assert(nv_storage_get_u8("value", &byte) == ESP_ERR_INVALID_STATE);
    assert(nv_storage_set_blob("blob", &blob, sizeof(blob)) ==
           ESP_ERR_INVALID_STATE);
    assert(nv_storage_get_blob("blob", &blob, &size) == ESP_ERR_INVALID_STATE);
    assert(nv_storage_erase_key("value") == ESP_ERR_INVALID_STATE);
    assert(nv_storage_blob_register("cfg", &blob, sizeof(blob), _default_magic,
                                    _validate_magic) == ESP_ERR_INVALID_STATE);
    assert(nv_storage_blob_load_all() == ESP_ERR_INVALID_STATE);
    assert(host_nvs_call_count(HOST_NVS_OPEN) == 0);

    host_nvs_fail_after(HOST_NVS_FLASH_INIT, 0,
                        ESP_ERR_NVS_NO_FREE_PAGES);
    assert(nv_storage_init() == ESP_ERR_NVS_NO_FREE_PAGES);
    assert(host_nvs_call_count(HOST_NVS_FLASH_ERASE) == 0);
    host_nvs_fail_after(HOST_NVS_FLASH_INIT, 0,
                        ESP_ERR_NVS_NEW_VERSION_FOUND);
    assert(nv_storage_init() == ESP_ERR_NVS_NEW_VERSION_FOUND);
    assert(host_nvs_call_count(HOST_NVS_FLASH_ERASE) == 0);

    assert(nv_storage_init() == ESP_OK);
    assert(nv_storage_init() == ESP_OK);
    assert(host_nvs_call_count(HOST_NVS_FLASH_INIT) == 3);
    assert(host_nvs_call_count(HOST_NVS_FLASH_ERASE) == 0);
    assert(nv_storage_deinit() == ESP_OK);
    assert(nv_storage_deinit() == ESP_OK);
    assert(host_nvs_call_count(HOST_NVS_FLASH_DEINIT) == 1);
}

static void _test_key_validation(void)
{
    _reset_initialized();
    static const char too_long[] = "1234567890123456";
    const char *invalid_keys[] = {NULL, "", too_long};
    uint8_t byte = 0;
    uint16_t half = 0;
    uint32_t word = 0;
    char text[8] = {0};
    size_t size;

    unsigned open_before = host_nvs_call_count(HOST_NVS_OPEN);
    for (size_t index = 0;
            index < sizeof(invalid_keys) / sizeof(invalid_keys[0]); index++)
    {
        const char *key = invalid_keys[index];
        size = sizeof(text);
        assert(nv_storage_set_u8(key, 1) == ESP_ERR_INVALID_ARG);
        assert(nv_storage_get_u8(key, &byte) == ESP_ERR_INVALID_ARG);
        assert(nv_storage_set_u16(key, 2) == ESP_ERR_INVALID_ARG);
        assert(nv_storage_get_u16(key, &half) == ESP_ERR_INVALID_ARG);
        assert(nv_storage_set_u32(key, 3) == ESP_ERR_INVALID_ARG);
        assert(nv_storage_get_u32(key, &word) == ESP_ERR_INVALID_ARG);
        assert(nv_storage_set_str(key, "ok") == ESP_ERR_INVALID_ARG);
        assert(nv_storage_get_str(key, text, &size) == ESP_ERR_INVALID_ARG);
        assert(nv_storage_set_blob(key, &word, sizeof(word)) ==
               ESP_ERR_INVALID_ARG);
        assert(nv_storage_get_blob(key, &word, &size) == ESP_ERR_INVALID_ARG);
        assert(nv_storage_erase_key(key) == ESP_ERR_INVALID_ARG);
        assert(nv_storage_blob_register(key, &word, sizeof(word), _default_magic,
                                        _validate_magic) == ESP_ERR_INVALID_ARG);
    }
    assert(host_nvs_call_count(HOST_NVS_OPEN) == open_before);
    assert(host_nvs_invalid_key_call_count() == 0);

    static const char max_key[] = "123456789012345";
    assert(nv_storage_set_u8(max_key, 91) == ESP_OK);
    assert(nv_storage_get_u8(max_key, &byte) == ESP_OK && byte == 91);
    assert(host_nvs_invalid_key_call_count() == 0);
    assert(nv_storage_deinit() == ESP_OK);
}

static void _test_scalar_operations_and_failures(void)
{
    _reset_initialized();
    uint8_t byte = 0;
    uint16_t half = 0;
    uint32_t word = 0;

    assert(nv_storage_set_u8("u8", 0x5a) == ESP_OK);
    assert(nv_storage_get_u8("u8", &byte) == ESP_OK && byte == 0x5a);
    assert(nv_storage_set_u16("u16", UINT16_C(0x55aa)) == ESP_OK);
    assert(nv_storage_get_u16("u16", &half) == ESP_OK &&
           half == UINT16_C(0x55aa));
    assert(nv_storage_set_u32("u32", UINT32_C(0x12345678)) == ESP_OK);
    assert(nv_storage_get_u32("u32", &word) == ESP_OK &&
           word == UINT32_C(0x12345678));

    assert(nv_storage_set_str("text", "microtech") == ESP_OK);
    size_t size = 0;
    assert(nv_storage_get_str("text", NULL, &size) ==
           ESP_ERR_NVS_INVALID_LENGTH);
    assert(size == strlen("microtech") + 1U);
    char small[4];
    size = sizeof(small);
    assert(nv_storage_get_str("text", small, &size) ==
           ESP_ERR_NVS_INVALID_LENGTH);
    char text[16];
    size = sizeof(text);
    assert(nv_storage_get_str("text", text, &size) == ESP_OK);
    assert(strcmp(text, "microtech") == 0 && size == strlen("microtech") + 1U);

    static const char grown_text[] = "microtech-expanded";
    assert(host_nvs_grow_before_nonnull_get(
               HOST_NVS_GET_STR, "text", grown_text, sizeof(grown_text)));
    size = sizeof(text);
    assert(nv_storage_get_str("text", text, &size) ==
           ESP_ERR_NVS_INVALID_LENGTH);
    assert(size == sizeof(grown_text));
    char grown_text_output[sizeof(grown_text)];
    size = sizeof(grown_text_output);
    assert(nv_storage_get_str("text", grown_text_output, &size) == ESP_OK);
    assert(size == sizeof(grown_text) &&
           strcmp(grown_text_output, grown_text) == 0);

    const uint8_t input_blob[] = {1, 2, 3, 4, 5};
    assert(nv_storage_set_blob("raw", input_blob, sizeof(input_blob)) == ESP_OK);
    size = 0;
    assert(nv_storage_get_blob("raw", NULL, &size) ==
           ESP_ERR_NVS_INVALID_LENGTH);
    assert(size == sizeof(input_blob));
    uint8_t output_blob[sizeof(input_blob)] = {0};
    size = sizeof(output_blob);
    assert(nv_storage_get_blob("raw", output_blob, &size) == ESP_OK);
    assert(size == sizeof(input_blob));
    assert(memcmp(input_blob, output_blob, sizeof(input_blob)) == 0);

    static const uint8_t grown_blob[] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(host_nvs_grow_before_nonnull_get(
               HOST_NVS_GET_BLOB, "raw", grown_blob, sizeof(grown_blob)));
    size = sizeof(output_blob);
    assert(nv_storage_get_blob("raw", output_blob, &size) ==
           ESP_ERR_NVS_INVALID_LENGTH);
    assert(size == sizeof(grown_blob));
    uint8_t grown_blob_output[sizeof(grown_blob)] = {0};
    size = sizeof(grown_blob_output);
    assert(nv_storage_get_blob("raw", grown_blob_output, &size) == ESP_OK);
    assert(size == sizeof(grown_blob) &&
           memcmp(grown_blob_output, grown_blob, sizeof(grown_blob)) == 0);

    assert(nv_storage_erase_key("u8") == ESP_OK);
    assert(nv_storage_get_u8("u8", &byte) == ESP_ERR_NVS_NOT_FOUND);
    assert(nv_storage_erase_key("u8") == ESP_ERR_NVS_NOT_FOUND);

    host_nvs_fail_after(HOST_NVS_OPEN, 0, ESP_FAIL);
    assert(nv_storage_set_u8("open-fail", 1) == ESP_FAIL);
    host_nvs_fail_after(HOST_NVS_SET_U8, 0, ESP_FAIL);
    unsigned commits = host_nvs_call_count(HOST_NVS_COMMIT);
    assert(nv_storage_set_u8("write-fail", 2) == ESP_FAIL);
    assert(host_nvs_call_count(HOST_NVS_COMMIT) == commits);
    host_nvs_fail_after(HOST_NVS_COMMIT, 0, ESP_FAIL);
    assert(nv_storage_set_u8("commit-fail", 3) == ESP_FAIL);
    assert(nv_storage_get_u8("commit-fail", &byte) == ESP_OK && byte == 3);
    assert(nv_storage_set_u8("erase-commit", 4) == ESP_OK);
    host_nvs_fail_after(HOST_NVS_COMMIT, 0, ESP_FAIL);
    assert(nv_storage_erase_key("erase-commit") == ESP_FAIL);
    assert(nv_storage_get_u8("erase-commit", &byte) == ESP_ERR_NVS_NOT_FOUND);
    host_nvs_fail_after(HOST_NVS_GET_U32, 0, ESP_FAIL);
    assert(nv_storage_get_u32("u32", &word) == ESP_FAIL);

    assert(nv_storage_get_u8("u8", NULL) == ESP_ERR_INVALID_ARG);
    assert(nv_storage_set_str("text", NULL) == ESP_ERR_INVALID_ARG);
    assert(nv_storage_get_str("text", text, NULL) == ESP_ERR_INVALID_ARG);
    assert(nv_storage_set_blob("raw", NULL, 1) == ESP_ERR_INVALID_ARG);
    assert(nv_storage_get_blob("raw", output_blob, NULL) == ESP_ERR_INVALID_ARG);
    assert(host_nvs_invalid_key_call_count() == 0);
    assert(nv_storage_deinit() == ESP_OK);
}

static void _test_blob_registry_ownership_duplicates_and_pool(void)
{
    _reset_initialized();
    uint32_t stack_value = 0;
    char stack_key[16] = "stack-key";
    assert(nv_storage_blob_register(stack_key, &stack_value,
                                    sizeof(stack_value), _default_magic,
                                    _validate_magic) == ESP_OK);
    memset(stack_key, 'x', strlen(stack_key));
    assert(nv_storage_blob_load_all() == ESP_OK);
    assert(stack_value == TEST_MAGIC);
    assert(nv_storage_blob_register("stack-key", &stack_value,
                                    sizeof(stack_value), _default_magic,
                                    _validate_magic) == ESP_OK);

    uint32_t conflict = 0;
    assert(nv_storage_blob_register("stack-key", &conflict, sizeof(conflict),
                                    _default_magic, _validate_magic) ==
           ESP_ERR_INVALID_STATE);
    assert(nv_storage_blob_register("stack-key", &stack_value,
                                    sizeof(stack_value), _default_second,
                                    _validate_magic) == ESP_ERR_INVALID_STATE);
    assert(nv_storage_blob_register("stack-key", &stack_value,
                                    sizeof(stack_value), _default_magic,
                                    _validate_never) == ESP_ERR_INVALID_STATE);
    assert(nv_storage_blob_register("zero", &conflict, 0, _default_magic,
                                    _validate_magic) == ESP_ERR_INVALID_ARG);

    uint32_t values[16] = {0};
    char keys[16][16];
    for (size_t index = 0; index < 15; index++)
    {
        (void)snprintf(keys[index], sizeof(keys[index]), "pool-%02u",
                       (unsigned)index);
        assert(nv_storage_blob_register(keys[index], &values[index],
                                        sizeof(values[index]), _default_magic,
                                        _validate_magic) == ESP_OK);
    }
    (void)snprintf(keys[15], sizeof(keys[15]), "overflow");
    assert(nv_storage_blob_register(keys[15], &values[15], sizeof(values[15]),
                                    _default_magic, _validate_magic) ==
           ESP_ERR_NO_MEM);

    assert(nv_storage_deinit() == ESP_OK);
    assert(nv_storage_init() == ESP_OK);
    unsigned opens = host_nvs_call_count(HOST_NVS_OPEN);
    assert(nv_storage_blob_load_all() == ESP_OK);
    assert(host_nvs_call_count(HOST_NVS_OPEN) == opens);
    assert(nv_storage_blob_register("stack-key", &conflict, sizeof(conflict),
                                    _default_second, _validate_never) == ESP_OK);
    assert(nv_storage_deinit() == ESP_OK);
}

static void _test_blob_default_and_validation_paths(void)
{
    _reset_initialized();
    uint32_t value = UINT32_C(0xaaaaaaaa);
    assert(nv_storage_blob_register("missing", &value, sizeof(value),
                                    _default_magic, _validate_magic) == ESP_OK);
    assert(nv_storage_blob_load_all() == ESP_OK);
    assert(value == TEST_MAGIC && s_default_calls == 1);
    value = 0;
    assert(nv_storage_blob_load_all() == ESP_OK);
    assert(value == TEST_MAGIC && s_default_calls == 1 && s_validate_calls == 1);

    _reset_initialized();
    uint16_t short_value = UINT16_C(0x1111);
    host_nvs_seed_blob("mismatch", &short_value, sizeof(short_value));
    value = UINT32_C(0xbbbbbbbb);
    assert(nv_storage_blob_register("mismatch", &value, sizeof(value),
                                    _default_magic, _validate_magic) == ESP_OK);
    assert(nv_storage_blob_load_all() == ESP_OK);
    assert(value == TEST_MAGIC && s_default_calls == 1);

    _reset_initialized();
    uint32_t corrupt = 0;
    host_nvs_seed_blob("corrupt", &corrupt, sizeof(corrupt));
    value = UINT32_C(0xcccccccc);
    assert(nv_storage_blob_register("corrupt", &value, sizeof(value),
                                    _default_magic, _validate_magic) == ESP_OK);
    assert(nv_storage_blob_load_all() == ESP_OK);
    assert(value == TEST_MAGIC && s_default_calls == 1 && s_validate_calls == 1);

    _reset_initialized();
    host_nvs_seed_blob("valid", &((uint32_t)
    {
        TEST_MAGIC
    }), sizeof(uint32_t));
    value = 0;
    assert(nv_storage_blob_register("valid", &value, sizeof(value),
                                    _default_magic, _validate_magic) == ESP_OK);
    assert(nv_storage_blob_load_all() == ESP_OK);
    assert(value == TEST_MAGIC && s_default_calls == 0 && s_validate_calls == 1);
    assert(nv_storage_deinit() == ESP_OK);
}

static void _assert_blob_load_failure_preserves_destination(
    host_nvs_operation_t operation, unsigned successes)
{
    _reset_initialized();
    uint32_t value = UINT32_C(0x77777777);
    if (operation == HOST_NVS_GET_BLOB && successes > 0)
    {
        host_nvs_seed_blob("failure", &((uint32_t)
        {
            TEST_MAGIC
        }),
        sizeof(uint32_t));
    }
    host_nvs_fail_after(operation, successes, ESP_FAIL);
    assert(nv_storage_blob_register("failure", &value, sizeof(value),
                                    _default_magic, _validate_magic) == ESP_OK);
    assert(nv_storage_blob_load_all() == ESP_FAIL);
    assert(value == UINT32_C(0x77777777));
    if (operation == HOST_NVS_COMMIT)
    {
        uint32_t stored = 0;
        size_t stored_size = sizeof(stored);
        assert(host_nvs_read_blob("failure", &stored, &stored_size));
        assert(stored_size == sizeof(stored) && stored == TEST_MAGIC);
    }
    assert(nv_storage_deinit() == ESP_OK);
}

static void _test_blob_failure_atomicity(void)
{
    _reset_initialized();
    uint32_t first = UINT32_C(0x11111111);
    uint32_t second = UINT32_C(0x22222222);
    assert(nv_storage_blob_register("first", &first, sizeof(first),
                                    _default_failure, _validate_magic) == ESP_OK);
    assert(nv_storage_blob_register("second", &second, sizeof(second),
                                    _default_second, _validate_magic) == ESP_OK);
    assert(nv_storage_blob_load_all() == ESP_FAIL);
    assert(first == UINT32_C(0x11111111));
    assert(second == UINT32_C(0x22222222));
    assert(s_default_calls == 1 && s_second_default_calls == 0);
    assert(nv_storage_deinit() == ESP_OK);

    _reset_initialized();
    uint32_t corrupt = 0;
    host_nvs_seed_blob("invalid-default", &corrupt, sizeof(corrupt));
    uint32_t invalid_destination = UINT32_C(0x44444444);
    assert(nv_storage_blob_register("invalid-default", &invalid_destination,
                                    sizeof(invalid_destination),
                                    _default_failure, _validate_magic) == ESP_OK);
    assert(nv_storage_blob_load_all() == ESP_FAIL);
    assert(invalid_destination == UINT32_C(0x44444444));
    assert(s_validate_calls == 1 && s_default_calls == 1);
    assert(nv_storage_deinit() == ESP_OK);

    _assert_blob_load_failure_preserves_destination(HOST_NVS_OPEN, 0);
    _assert_blob_load_failure_preserves_destination(HOST_NVS_GET_BLOB, 0);
    _assert_blob_load_failure_preserves_destination(HOST_NVS_GET_BLOB, 1);
    _assert_blob_load_failure_preserves_destination(HOST_NVS_SET_BLOB, 0);
    _assert_blob_load_failure_preserves_destination(HOST_NVS_COMMIT, 0);

    _reset_initialized();
    uint32_t value = UINT32_C(0x33333333);
    assert(nv_storage_blob_register("default-fail", &value, sizeof(value),
                                    _default_failure, _validate_magic) == ESP_OK);
    assert(nv_storage_blob_load_all() == ESP_FAIL);
    assert(value == UINT32_C(0x33333333));
    size_t size = sizeof(value);
    assert(!host_nvs_read_blob("default-fail", &value, &size));
    assert(nv_storage_deinit() == ESP_OK);
}

static void _test_blob_candidate_allocation_failure(void)
{
    _reset_initialized();
    uint8_t destination = 0x5a;
    assert(nv_storage_blob_register("alloc-failure", &destination, SIZE_MAX,
                                    _default_failure, NULL) == ESP_OK);
    assert(nv_storage_blob_load_all() == ESP_ERR_NO_MEM);
    assert(destination == 0x5a && s_default_calls == 0);

    uint32_t retry = 0;
    assert(nv_storage_blob_register("after-alloc", &retry, sizeof(retry),
                                    _default_magic, _validate_magic) == ESP_OK);
    assert(nv_storage_deinit() == ESP_OK);
}

typedef struct thread_result
{
    esp_err_t result;
    atomic_bool entered;
    atomic_bool finished;
} thread_result_t;

static void *_blocked_set_thread(void *context)
{
    thread_result_t *thread = context;
    atomic_store(&thread->entered, true);
    thread->result = nv_storage_set_u8("blocked", 42);
    atomic_store(&thread->finished, true);
    return NULL;
}

static void *_deinit_thread(void *context)
{
    thread_result_t *thread = context;
    atomic_store(&thread->entered, true);
    thread->result = nv_storage_deinit();
    atomic_store(&thread->finished, true);
    return NULL;
}

static void *_load_thread(void *context)
{
    thread_result_t *thread = context;
    atomic_store(&thread->entered, true);
    thread->result = nv_storage_blob_load_all();
    atomic_store(&thread->finished, true);
    return NULL;
}

typedef struct register_thread
{
    thread_result_t thread;
    const char *key;
    uint32_t *data;
} register_thread_t;

static void *_register_thread(void *context)
{
    register_thread_t *registration = context;
    atomic_store(&registration->thread.entered, true);
    registration->thread.result = nv_storage_blob_register(
                                      registration->key, registration->data,
                                      sizeof(*registration->data), _default_magic,
                                      _validate_magic);
    atomic_store(&registration->thread.finished, true);
    return NULL;
}

static void _thread_result_init(thread_result_t *result)
{
    result->result = ESP_FAIL;
    atomic_init(&result->entered, false);
    atomic_init(&result->finished, false);
}

static void _wait_for_thread_entry(thread_result_t *result)
{
    for (unsigned attempt = 0; attempt < 1000 &&
            !atomic_load(&result->entered); attempt++)
    {
        _sleep_ms(1);
    }
    assert(atomic_load(&result->entered));
}

static bool _thread_finished_within(thread_result_t *result,
                                    uint32_t timeout_ms)
{
    for (uint32_t attempt = 0; attempt < timeout_ms &&
            !atomic_load(&result->finished); attempt++)
    {
        _sleep_ms(1);
    }
    return atomic_load(&result->finished);
}

static void _wait_for_thread_finish(thread_result_t *result)
{
    assert(_thread_finished_within(result, 1000));
}

static void _test_active_access_barrier(void)
{
    _reset_initialized();
    host_nvs_block_open(true);

    thread_result_t setter;
    _thread_result_init(&setter);
    pthread_t setter_id;
    assert(pthread_create(&setter_id, NULL, _blocked_set_thread, &setter) == 0);
    assert(host_nvs_wait_for_blocked_open(1000));

    thread_result_t cleanup;
    _thread_result_init(&cleanup);
    pthread_t cleanup_id;
    assert(pthread_create(&cleanup_id, NULL, _deinit_thread, &cleanup) == 0);
    _wait_for_thread_entry(&cleanup);
    _sleep_ms(25);
    assert(!atomic_load(&cleanup.finished));
    assert(host_nvs_call_count(HOST_NVS_FLASH_DEINIT) == 0);

    host_nvs_block_open(false);
    assert(pthread_join(setter_id, NULL) == 0);
    assert(pthread_join(cleanup_id, NULL) == 0);
    assert(setter.result == ESP_OK);
    assert(cleanup.result == ESP_OK);
    assert(host_nvs_call_count(HOST_NVS_FLASH_DEINIT) == 1);
}

static void _test_load_admission_precedes_registry_lock(void)
{
    _reset_initialized();
    uint32_t value = 0;
    assert(nv_storage_blob_register("admission", &value, sizeof(value),
                                    _default_magic, _validate_magic) == ESP_OK);

    host_freertos_block_next_semaphore_take();
    thread_result_t loader;
    _thread_result_init(&loader);
    pthread_t loader_id;
    assert(pthread_create(&loader_id, NULL, _load_thread, &loader) == 0);
    assert(host_freertos_wait_for_blocked_semaphore_take(1000));

    thread_result_t cleanup;
    _thread_result_init(&cleanup);
    pthread_t cleanup_id;
    assert(pthread_create(&cleanup_id, NULL, _deinit_thread, &cleanup) == 0);
    _wait_for_thread_entry(&cleanup);
    bool cleanup_rejected = _thread_finished_within(&cleanup, 500);

    host_freertos_release_blocked_semaphore_take();
    assert(pthread_join(loader_id, NULL) == 0);
    assert(pthread_join(cleanup_id, NULL) == 0);

    assert(cleanup_rejected);
    assert(cleanup.result == ESP_ERR_INVALID_STATE);
    assert(loader.result == ESP_OK && value == TEST_MAGIC);
    assert(nv_storage_deinit() == ESP_OK);
}

static void _test_registration_precedes_load_mutex_boundary(void)
{
    _reset_initialized();
    uint32_t existing = 0;
    uint32_t late = 0;
    assert(nv_storage_blob_register("existing", &existing, sizeof(existing),
                                    _default_magic, _validate_magic) == ESP_OK);

    host_freertos_block_after_next_semaphore_take();
    register_thread_t registration =
    {
        .key = "late",
        .data = &late,
    };
    _thread_result_init(&registration.thread);
    pthread_t registration_id;
    assert(pthread_create(&registration_id, NULL, _register_thread,
                          &registration) == 0);
    assert(host_freertos_wait_for_blocked_after_semaphore_take(1000));

    thread_result_t loader;
    _thread_result_init(&loader);
    pthread_t loader_id;
    assert(pthread_create(&loader_id, NULL, _load_thread, &loader) == 0);
    assert(host_freertos_wait_for_pending_semaphore_take(1000));

    host_freertos_release_blocked_after_semaphore_take();
    assert(pthread_join(registration_id, NULL) == 0);
    assert(pthread_join(loader_id, NULL) == 0);

    assert(registration.thread.result == ESP_ERR_INVALID_STATE);
    assert(loader.result == ESP_OK && existing == TEST_MAGIC && late == 0);
    assert(nv_storage_blob_register("late", &late, sizeof(late),
                                    _default_magic, _validate_magic) == ESP_OK);
    assert(nv_storage_deinit() == ESP_OK);
}

static void _test_callback_reentry_rejected(void)
{
    _reset_initialized();
    uint32_t value = 0;
    assert(nv_storage_blob_register("reentrant", &value, sizeof(value),
                                    _default_reentrant, _validate_magic) == ESP_OK);

    thread_result_t loader;
    _thread_result_init(&loader);
    pthread_t loader_id;
    host_nvs_block_open(true);
    assert(pthread_create(&loader_id, NULL, _load_thread, &loader) == 0);
    assert(host_nvs_wait_for_blocked_open(1000));
    assert(s_default_calls == 0);
    assert(nv_storage_deinit() == ESP_ERR_INVALID_STATE);
    assert(nv_storage_init() == ESP_ERR_INVALID_STATE);

    host_nvs_block_open(false);
    _wait_for_thread_finish(&loader);
    assert(pthread_join(loader_id, NULL) == 0);

    assert(loader.result == ESP_OK);
    assert(value == TEST_MAGIC);
    assert(s_reentrant_register_result == ESP_ERR_INVALID_STATE);
    assert(s_reentrant_load_result == ESP_ERR_INVALID_STATE);
    assert(s_reentrant_init_result == ESP_ERR_INVALID_STATE);
    assert(s_reentrant_deinit_result == ESP_ERR_INVALID_STATE);
    assert(s_reentrant_scalar_result == ESP_OK);
    uint8_t scalar = 0;
    assert(nv_storage_get_u8("callback-scalar", &scalar) == ESP_OK && scalar == 9);
    assert(nv_storage_set_u8("after-reentry", 7) == ESP_OK);
    assert(nv_storage_deinit() == ESP_OK);
}

static void _test_registry_load_rejects_concurrent_operations(void)
{
    _reset_initialized();
    uint32_t first = 0;
    assert(nv_storage_blob_register("blocking", &first, sizeof(first),
                                    _default_blocking, _validate_magic) == ESP_OK);

    thread_result_t loader;
    _thread_result_init(&loader);
    pthread_t loader_id;
    assert(pthread_create(&loader_id, NULL, _load_thread, &loader) == 0);

    (void)pthread_mutex_lock(&s_callback_lock);
    while (!s_blocking_default_entered)
    {
        (void)pthread_cond_wait(&s_callback_changed, &s_callback_lock);
    }
    (void)pthread_mutex_unlock(&s_callback_lock);

    assert(nv_storage_deinit() == ESP_ERR_INVALID_STATE);

    uint32_t second = 0;
    register_thread_t registration =
    {
        .key = "serialized",
        .data = &second,
    };
    _thread_result_init(&registration.thread);
    pthread_t registration_id;
    assert(pthread_create(&registration_id, NULL, _register_thread,
                          &registration) == 0);
    _wait_for_thread_entry(&registration.thread);
    _wait_for_thread_finish(&registration.thread);
    assert(pthread_join(registration_id, NULL) == 0);
    assert(registration.thread.result == ESP_ERR_INVALID_STATE);

    (void)pthread_mutex_lock(&s_callback_lock);
    s_blocking_default_release = true;
    (void)pthread_cond_broadcast(&s_callback_changed);
    (void)pthread_mutex_unlock(&s_callback_lock);
    assert(pthread_join(loader_id, NULL) == 0);
    assert(loader.result == ESP_OK);
    assert(first == TEST_MAGIC);
    assert(nv_storage_blob_register("serialized", &second, sizeof(second),
                                    _default_magic, _validate_magic) == ESP_OK);
    assert(nv_storage_deinit() == ESP_OK);
}

static void _test_deinit_failure_retry_and_registry_reset(void)
{
    _reset_initialized();
    uint32_t value = 0;
    assert(nv_storage_blob_register("retry", &value, sizeof(value),
                                    _default_magic, _validate_magic) == ESP_OK);
    host_nvs_fail_after(HOST_NVS_FLASH_DEINIT, 0, ESP_FAIL);
    assert(nv_storage_deinit() == ESP_FAIL);
    assert(nv_storage_init() == ESP_ERR_INVALID_STATE);
    assert(nv_storage_set_u8("value", 1) == ESP_ERR_INVALID_STATE);
    assert(nv_storage_blob_load_all() == ESP_ERR_INVALID_STATE);
    assert(nv_storage_blob_register("other", &value, sizeof(value),
                                    _default_magic, _validate_magic) ==
           ESP_ERR_INVALID_STATE);

    assert(nv_storage_deinit() == ESP_OK);
    assert(host_nvs_call_count(HOST_NVS_FLASH_DEINIT) == 2);
    assert(nv_storage_deinit() == ESP_OK);
    assert(host_nvs_call_count(HOST_NVS_FLASH_DEINIT) == 2);

    assert(nv_storage_init() == ESP_OK);
    unsigned opens = host_nvs_call_count(HOST_NVS_OPEN);
    assert(nv_storage_blob_load_all() == ESP_OK);
    assert(host_nvs_call_count(HOST_NVS_OPEN) == opens);
    assert(nv_storage_blob_register("retry", &value, sizeof(value),
                                    _default_second, _validate_never) == ESP_OK);
    assert(nv_storage_deinit() == ESP_OK);
}

int main(void)
{
    _test_init_policy_and_uninitialized_rejection();
    _test_key_validation();
    _test_scalar_operations_and_failures();
    _test_blob_registry_ownership_duplicates_and_pool();
    _test_blob_default_and_validation_paths();
    _test_blob_failure_atomicity();
    _test_blob_candidate_allocation_failure();
    _test_active_access_barrier();
    _test_load_admission_precedes_registry_lock();
    _test_registration_precedes_load_mutex_boundary();
    _test_callback_reentry_rejected();
    _test_registry_load_rejects_concurrent_operations();
    _test_deinit_failure_retry_and_registry_reset();
    _reset_uninitialized();
    puts("nv_storage production-source tests passed");
    return 0;
}

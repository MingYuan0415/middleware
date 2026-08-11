#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "ble_nimble_store_reset.h"

#define TEST_TRACE_CAPACITY 4U

typedef enum fake_step
{
    FAKE_STEP_ERASE = 1,
    FAKE_STEP_CLEAR,
    FAKE_STEP_AUDIT,
} fake_step_t;

typedef struct fake_reset
{
    fake_step_t trace[TEST_TRACE_CAPACITY];
    size_t trace_count;
    size_t fail_call;
    esp_err_t fail_result;
} fake_reset_t;

static esp_err_t _record(fake_reset_t *fake, fake_step_t step)
{
    assert(fake != NULL);
    assert(fake->trace_count < TEST_TRACE_CAPACITY);
    fake->trace[fake->trace_count] = step;
    fake->trace_count++;
    return fake->trace_count == fake->fail_call ?
           fake->fail_result : ESP_OK;
}

static esp_err_t _erase(void *context)
{
    return _record(context, FAKE_STEP_ERASE);
}

static esp_err_t _clear(void *context)
{
    return _record(context, FAKE_STEP_CLEAR);
}

static esp_err_t _audit(void *context)
{
    return _record(context, FAKE_STEP_AUDIT);
}

static ble_nimble_store_reset_ops_t _ops(fake_reset_t *fake)
{
    const ble_nimble_store_reset_ops_t ops =
    {
        .erase_namespace = _erase,
        .clear_runtime = _clear,
        .audit_empty = _audit,
        .context = fake,
    };

    return ops;
}

static void test_reset_order(void)
{
    fake_reset_t fake = {0};
    const ble_nimble_store_reset_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_reset_run(&ops) == ESP_OK);
    assert(fake.trace_count == 4U);
    assert(fake.trace[0] == FAKE_STEP_ERASE);
    assert(fake.trace[1] == FAKE_STEP_CLEAR);
    assert(fake.trace[2] == FAKE_STEP_ERASE);
    assert(fake.trace[3] == FAKE_STEP_AUDIT);
}

static void test_each_failure_stops_the_reset(void)
{
    for (size_t fail_call = 1U; fail_call <= TEST_TRACE_CAPACITY; ++fail_call)
    {
        fake_reset_t fake =
        {
            .fail_call = fail_call,
            .fail_result = ESP_FAIL,
        };
        const ble_nimble_store_reset_ops_t ops = _ops(&fake);

        assert(ble_nimble_store_reset_run(&ops) == ESP_FAIL);
        assert(fake.trace_count == fail_call);
    }
}

static void test_invalid_configuration_is_rejected(void)
{
    fake_reset_t fake = {0};
    ble_nimble_store_reset_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_reset_run(NULL) == ESP_ERR_INVALID_ARG);
    ops.erase_namespace = NULL;
    assert(ble_nimble_store_reset_run(&ops) == ESP_ERR_INVALID_ARG);
    ops = _ops(&fake);
    ops.clear_runtime = NULL;
    assert(ble_nimble_store_reset_run(&ops) == ESP_ERR_INVALID_ARG);
    ops = _ops(&fake);
    ops.audit_empty = NULL;
    assert(ble_nimble_store_reset_run(&ops) == ESP_ERR_INVALID_ARG);
    assert(fake.trace_count == 0U);
}

int main(void)
{
    test_reset_order();
    test_each_failure_stops_the_reset();
    test_invalid_configuration_is_rejected();
    puts("ble_nimble_store_reset host tests passed");
    return 0;
}

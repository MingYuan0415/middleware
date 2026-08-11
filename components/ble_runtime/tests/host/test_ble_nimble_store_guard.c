#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "ble_nimble_store_guard.h"

typedef struct fake_store
{
    unsigned int delete_calls;
    unsigned int write_calls;
    bool ram_present;
    bool persisted_present;
    bool fail_persist;
} fake_store_t;

union ble_store_value
{
    unsigned int unused;
};

static int _original_writer(
    int object_type, const union ble_store_value *value)
{
    (void)object_type;
    (void)value;
    return 0;
}

static int _guarded_writer(
    int object_type, const union ble_store_value *value)
{
    (void)object_type;
    (void)value;
    return 0;
}

static int _foreign_writer(
    int object_type, const union ble_store_value *value)
{
    (void)object_type;
    (void)value;
    return 0;
}

static int _write_peer(
    ble_nimble_store_guard_t *guard, fake_store_t *store,
    int write_result, int retryable_capacity_result)
{
    if (ble_nimble_store_guard_error(guard) != ESP_OK)
    {
        return -1;
    }
    store->write_calls++;
    store->ram_present = true;
    if (write_result == 0)
    {
        store->persisted_present = true;
    }
    return ble_nimble_store_guard_record_write_result(
               guard, write_result, retryable_capacity_result);
}

static esp_err_t _delete_peer(
    ble_nimble_store_guard_t *guard, fake_store_t *store)
{
    const esp_err_t admitted = ble_nimble_store_guard_error(guard);

    if (admitted != ESP_OK)
    {
        return admitted;
    }
    store->delete_calls++;
    store->ram_present = false;
    const esp_err_t result = store->fail_persist ? ESP_FAIL : ESP_OK;

    if (result == ESP_OK)
    {
        store->persisted_present = false;
    }
    ble_nimble_store_guard_finish(guard, result);
    return result;
}

static void test_persist_failure_blocks_same_host_absent_retry(void)
{
    ble_nimble_store_guard_t guard;
    fake_store_t store =
    {
        .ram_present = true,
        .persisted_present = true,
        .fail_persist = true,
    };

    ble_nimble_store_guard_reset(&guard);
    assert(_delete_peer(&guard, &store) == ESP_FAIL);
    assert(store.delete_calls == 1U);
    assert(!store.ram_present);
    assert(store.persisted_present);

    /* The failed IDF persistence removed the RAM entry. A same-run retry
     * would observe absence and falsely claim success, so the sticky guard
     * must reject it without invoking the store again. */
    store.fail_persist = false;
    assert(_delete_peer(&guard, &store) == ESP_FAIL);
    assert(store.delete_calls == 1U);
    assert(store.persisted_present);

    /* A fresh host run reloads the durable record before retrying. */
    ble_nimble_store_guard_reset(&guard);
    store.ram_present = store.persisted_present;
    assert(_delete_peer(&guard, &store) == ESP_OK);
    assert(store.delete_calls == 2U);
    assert(!store.ram_present);
    assert(!store.persisted_present);
}

static void test_first_failure_is_immutable(void)
{
    ble_nimble_store_guard_t guard;

    ble_nimble_store_guard_reset(&guard);
    ble_nimble_store_guard_finish(&guard, ESP_ERR_NO_MEM);
    ble_nimble_store_guard_finish(&guard, ESP_FAIL);
    assert(ble_nimble_store_guard_error(&guard) == ESP_ERR_NO_MEM);
}

static void test_write_persist_failure_cannot_verify_ram_mirror(void)
{
    static const int capacity_result = 7;
    static const int persist_error = 8;
    ble_nimble_store_guard_t guard;
    fake_store_t store = {0};

    ble_nimble_store_guard_reset(&guard);
    assert(_write_peer(
               &guard, &store, capacity_result, capacity_result) ==
           capacity_result);
    assert(ble_nimble_store_guard_error(&guard) == ESP_OK);

    /* The IDF writer updates its RAM mirror before NVS persistence. A failed
     * persist must latch even though a subsequent read sees the RAM entry. */
    assert(_write_peer(
               &guard, &store, persist_error, capacity_result) ==
           persist_error);
    assert(store.ram_present);
    assert(!store.persisted_present);
    assert(ble_nimble_store_guard_error(&guard) == ESP_FAIL);

    assert(_write_peer(&guard, &store, 0, capacity_result) == -1);
    assert(store.write_calls == 2U);
    assert(!store.persisted_present);
}

static void test_writer_guard_survives_startup_and_reset_resync(void)
{
    ble_nimble_store_callback_guard_t callback_guard = {0};
    ble_nimble_store_write_callback_t *callback = NULL;

    assert(ble_nimble_store_callback_guard_install(
               &callback_guard, &callback, _guarded_writer) == ESP_OK);
    assert(callback == NULL);
    assert(callback_guard.active);
    assert(callback_guard.original == NULL);

    /* Cold boot installs the IDF writer during NimBLE privacy startup. The
     * first sync captures it and installs the guard. */
    callback = _original_writer;
    assert(ble_nimble_store_callback_guard_reconcile(
               &callback_guard, &callback, _guarded_writer) == ESP_OK);
    assert(callback == _guarded_writer);
    assert(callback_guard.original == _original_writer);

    /* Every host-reset resync restores the same IDF writer before sync. */
    callback = _original_writer;
    assert(ble_nimble_store_callback_guard_reconcile(
               &callback_guard, &callback, _guarded_writer) == ESP_OK);
    assert(callback == _guarded_writer);

    /* Reconciliation is also idempotent when IDF leaves the guard installed. */
    assert(ble_nimble_store_callback_guard_reconcile(
               &callback_guard, &callback, _guarded_writer) == ESP_OK);
    assert(callback == _guarded_writer);

    ble_nimble_store_callback_guard_restore(
        &callback_guard, &callback, _guarded_writer);
    assert(callback == _original_writer);
    assert(!callback_guard.active);
    assert(callback_guard.original == NULL);
}

static void test_writer_guard_never_overwrites_foreign_callback(void)
{
    ble_nimble_store_callback_guard_t callback_guard = {0};
    ble_nimble_store_write_callback_t *callback = NULL;

    assert(ble_nimble_store_callback_guard_install(
               &callback_guard, &callback, _guarded_writer) == ESP_OK);
    callback = _original_writer;
    assert(ble_nimble_store_callback_guard_reconcile(
               &callback_guard, &callback, _guarded_writer) == ESP_OK);
    callback = _foreign_writer;
    assert(ble_nimble_store_callback_guard_reconcile(
               &callback_guard, &callback, _guarded_writer) ==
           ESP_ERR_INVALID_STATE);
    assert(callback == _foreign_writer);

    ble_nimble_store_callback_guard_restore(
        &callback_guard, &callback, _guarded_writer);
    assert(callback == _foreign_writer);
    assert(!callback_guard.active);
    assert(callback_guard.original == NULL);
}

static void test_writer_guard_rollback_before_startup_sync(void)
{
    ble_nimble_store_callback_guard_t callback_guard = {0};
    ble_nimble_store_write_callback_t *callback = NULL;

    assert(ble_nimble_store_callback_guard_install(
               &callback_guard, &callback, _guarded_writer) == ESP_OK);
    ble_nimble_store_callback_guard_restore(
        &callback_guard, &callback, _guarded_writer);
    assert(callback == NULL);
    assert(!callback_guard.active);
    assert(callback_guard.original == NULL);
}

static void test_writer_guard_teardown_accepts_uncaptured_original(void)
{
    ble_nimble_store_callback_guard_t callback_guard = {0};
    ble_nimble_store_write_callback_t *callback = NULL;

    assert(ble_nimble_store_callback_guard_install(
               &callback_guard, &callback, _guarded_writer) == ESP_OK);
    callback = _original_writer;

    /* Startup can fail after IDF restores its callback but before sync gives
     * the port a chance to capture and wrap it. Rollback must preserve the
     * uncaptured callback and release the lifecycle state. */
    ble_nimble_store_callback_guard_restore(
        &callback_guard, &callback, _guarded_writer);
    assert(callback == _original_writer);
    assert(!callback_guard.active);
    assert(callback_guard.original == NULL);
}

int main(void)
{
    test_persist_failure_blocks_same_host_absent_retry();
    test_first_failure_is_immutable();
    test_write_persist_failure_cannot_verify_ram_mirror();
    test_writer_guard_survives_startup_and_reset_resync();
    test_writer_guard_never_overwrites_foreign_callback();
    test_writer_guard_rollback_before_startup_sync();
    test_writer_guard_teardown_accepts_uncaptured_original();
    return 0;
}

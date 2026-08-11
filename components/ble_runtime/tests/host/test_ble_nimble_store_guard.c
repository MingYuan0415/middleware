#include <assert.h>
#include <stdbool.h>

#include "ble_nimble_store_guard.h"

typedef struct fake_store
{
    unsigned int delete_calls;
    bool ram_present;
    bool persisted_present;
    bool fail_persist;
} fake_store_t;

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

int main(void)
{
    test_persist_failure_blocks_same_host_absent_retry();
    test_first_failure_is_immutable();
    return 0;
}

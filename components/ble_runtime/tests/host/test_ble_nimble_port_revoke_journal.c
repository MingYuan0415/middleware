#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ble_nimble_port_revoke_journal.h"
#include "nv_storage.h"

#define TEST_JOURNAL_KEY "ble.revoke"
#define TEST_JOURNAL_MARKER_SIZE 16U

static void _test_absent(void)
{
    bool pending = true;

    nv_storage_fake_reset();
    assert(ble_nimble_port_revoke_journal_pending(&pending) == ESP_OK);
    assert(!pending);
    assert(ble_nimble_port_revoke_journal_pending(NULL) ==
           ESP_ERR_INVALID_ARG);
    assert(ble_nimble_port_revoke_journal_end() == ESP_OK);
}

static void _test_begin_pending_end(void)
{
    bool pending = false;

    nv_storage_fake_reset();
    assert(ble_nimble_port_revoke_journal_begin() == ESP_OK);
    assert(ble_nimble_port_revoke_journal_pending(&pending) == ESP_OK);
    assert(pending);
    assert(ble_nimble_port_revoke_journal_begin() == ESP_OK);
    assert(ble_nimble_port_revoke_journal_pending(&pending) == ESP_OK);
    assert(pending);
    assert(ble_nimble_port_revoke_journal_end() == ESP_OK);
    assert(ble_nimble_port_revoke_journal_pending(&pending) == ESP_OK);
    assert(!pending);
    assert(ble_nimble_port_revoke_journal_end() == ESP_OK);
}

static void _test_malformed(void)
{
    uint8_t marker[TEST_JOURNAL_MARKER_SIZE];
    uint8_t short_marker[4];
    uint8_t oversized_marker[TEST_JOURNAL_MARKER_SIZE + 1U];
    bool pending = true;

    nv_storage_fake_reset();
    memset(marker, 0, sizeof(marker));
    assert(nv_storage_set_blob(TEST_JOURNAL_KEY, marker,
                               sizeof(marker)) == ESP_OK);
    assert(ble_nimble_port_revoke_journal_pending(&pending) ==
           ESP_ERR_INVALID_RESPONSE);
    assert(!pending);

    memset(short_marker, 0, sizeof(short_marker));
    assert(nv_storage_set_blob(TEST_JOURNAL_KEY, short_marker,
                               sizeof(short_marker)) == ESP_OK);
    assert(ble_nimble_port_revoke_journal_pending(&pending) ==
           ESP_ERR_INVALID_RESPONSE);

    assert(ble_nimble_port_revoke_journal_begin() == ESP_OK);
    assert(ble_nimble_port_revoke_journal_pending(&pending) == ESP_OK);
    assert(pending);

    memset(oversized_marker, 0, sizeof(oversized_marker));
    assert(nv_storage_set_blob(TEST_JOURNAL_KEY, oversized_marker,
                               sizeof(oversized_marker)) == ESP_OK);
    assert(ble_nimble_port_revoke_journal_pending(&pending) ==
           ESP_ERR_INVALID_RESPONSE);
    assert(!pending);
    assert(ble_nimble_port_revoke_journal_begin() == ESP_OK);
    assert(ble_nimble_port_revoke_journal_pending(&pending) == ESP_OK);
    assert(pending);
}

static void _test_storage_errors(void)
{
    bool pending = false;

    nv_storage_fake_reset();
    nv_storage_fake_fail_next_get(ESP_FAIL);
    assert(ble_nimble_port_revoke_journal_pending(&pending) == ESP_FAIL);
    nv_storage_fake_fail_next_get(ESP_FAIL);
    assert(ble_nimble_port_revoke_journal_begin() == ESP_FAIL);
    assert(ble_nimble_port_revoke_journal_pending(&pending) == ESP_OK);
    assert(!pending);
}

int main(void)
{
    _test_absent();
    _test_begin_pending_end();
    _test_malformed();
    _test_storage_errors();
    puts("ble nimble port revoke journal tests passed");
    return 0;
}

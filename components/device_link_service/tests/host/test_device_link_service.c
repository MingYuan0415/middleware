#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "ble_link_service.h"
#include "ble_nimble_port.h"
#include "ble_nimble_port_revoke_journal.h"
#include "device_link_confirmation.h"
#include "device_link_revoke_progress.h"
#include "device_link_service.h"

static esp_err_t s_revoke_result;
static esp_err_t s_journal_result;
static bool s_journal_pending;
static unsigned int s_revoke_calls;
static unsigned int s_journal_calls;

esp_err_t ble_nimble_port_revoke_binding(void)
{
    ++s_revoke_calls;
    return s_revoke_result;
}

esp_err_t ble_nimble_port_revoke_journal_pending(bool *out_pending)
{
    assert(out_pending != NULL);
    ++s_journal_calls;
    *out_pending = s_journal_pending;
    return s_journal_result;
}

static void _reset_revoke_fake(void)
{
    s_revoke_result = ESP_OK;
    s_journal_result = ESP_OK;
    s_journal_pending = false;
    s_revoke_calls = 0U;
    s_journal_calls = 0U;
}

static void test_revoke_progress_waits_for_journal_completion(void)
{
    _reset_revoke_fake();
    s_journal_pending = true;
    assert(device_link_revoke_progress() == ESP_ERR_NOT_FINISHED);
    assert(device_link_revoke_progress() == ESP_ERR_NOT_FINISHED);
    assert(s_revoke_calls == 2U);
    assert(s_journal_calls == 2U);

    s_journal_pending = false;
    assert(device_link_revoke_progress() == ESP_OK);
    assert(s_revoke_calls == 3U);
    assert(s_journal_calls == 3U);
}

static void test_revoke_progress_propagates_failures(void)
{
    _reset_revoke_fake();
    s_revoke_result = ESP_ERR_INVALID_STATE;
    assert(device_link_revoke_progress() == ESP_ERR_INVALID_STATE);
    assert(s_revoke_calls == 1U);
    assert(s_journal_calls == 0U);

    _reset_revoke_fake();
    s_journal_result = ESP_FAIL;
    assert(device_link_revoke_progress() == ESP_FAIL);
    assert(s_revoke_calls == 1U);
    assert(s_journal_calls == 1U);
}

static void test_status_exposes_numeric_comparison(void)
{
    device_link_service_status_t status;

    memset(&status, 0, sizeof(status));
    status.bound = true;
    status.pending_confirmation = true;
    status.numeric_comparison = 123456U;
    status.confirmation_token = 7U;
    assert(status.bound);
    assert(status.pending_confirmation);
    assert(status.numeric_comparison == 123456U);
    assert(status.confirmation_token == 7U);
    assert(offsetof(device_link_service_status_t, numeric_comparison) > 0U);
}

static void test_confirmation_sync_reports_only_observable_changes(void)
{
    device_link_service_status_t status;
    ble_link_confirmation_snapshot_t confirmation;

    memset(&status, 0, sizeof(status));
    memset(&confirmation, 0, sizeof(confirmation));
    assert(!device_link_confirmation_sync(&status, &confirmation));

    confirmation.pending = true;
    confirmation.token = UINT64_C(0x1020304050607080);
    confirmation.numeric_comparison = 123456U;
    assert(device_link_confirmation_sync(&status, &confirmation));
    assert(status.pending_confirmation);
    assert(status.confirmation_token == confirmation.token);
    assert(status.numeric_comparison == 123456U);
    assert(!device_link_confirmation_sync(&status, &confirmation));

    confirmation.token++;
    confirmation.numeric_comparison = 654321U;
    assert(device_link_confirmation_sync(&status, &confirmation));
    assert(status.confirmation_token == confirmation.token);
    assert(status.numeric_comparison == 654321U);

    memset(&confirmation, 0, sizeof(confirmation));
    assert(device_link_confirmation_sync(&status, &confirmation));
    assert(!status.pending_confirmation);
    assert(status.confirmation_token == 0U);
    assert(status.numeric_comparison == 0U);
}

int main(void)
{
    test_revoke_progress_waits_for_journal_completion();
    test_revoke_progress_propagates_failures();
    test_status_exposes_numeric_comparison();
    test_confirmation_sync_reports_only_observable_changes();
    return 0;
}

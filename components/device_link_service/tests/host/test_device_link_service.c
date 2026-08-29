#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "ble_link_service.h"
#include "device_link_confirmation.h"
#include "device_link_service.h"

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
    test_status_exposes_numeric_comparison();
    test_confirmation_sync_reports_only_observable_changes();
    return 0;
}

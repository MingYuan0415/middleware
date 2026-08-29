#include "device_link_confirmation.h"

bool device_link_confirmation_sync(
    device_link_service_status_t *status,
    const ble_link_confirmation_snapshot_t *confirmation)
{
    const bool changed =
        status->pending_confirmation != confirmation->pending ||
        status->confirmation_token != confirmation->token ||
        status->numeric_comparison != confirmation->numeric_comparison;

    status->pending_confirmation = confirmation->pending;
    status->confirmation_token = confirmation->token;
    status->numeric_comparison = confirmation->numeric_comparison;
    return changed;
}

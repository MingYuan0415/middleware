#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
middleware_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
pattern='ble_link_gatt_(refresh_link_state|authentication_epoch_advance|cccd_epoch_advance|mark_link_state_dirty|request_link_state_refresh|link_state_dirty|link_state_retry_pending|link_state_retry_remaining_ms|link_state_handle|control_tx_handle|control_rx_handle)'

if find "$middleware_root/components" -type f \( -name '*.c' -o -name '*.h' \) \
        -exec grep -nE "$pattern" {} +; then
    echo "Legacy Device Link GATT interface remains" >&2
    exit 1
fi

echo "Legacy Device Link GATT interfaces absent"

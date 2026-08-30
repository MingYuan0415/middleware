#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_path="$script_dir/../src/ble_nimble_port.c"

if [ ! -f "$source_path" ]; then
    echo "BLE runtime source missing: $source_path" >&2
    exit 1
fi

connection_callback=$(awk '
    /static int _ble_nimble_port_gap_connection_event\(/ { in_function = 1 }
    in_function { print }
    in_function && /^}/ { exit }
' "$source_path")

printf '%s\n' "$connection_callback" | grep -F \
    'BLE_GAP_EVENT_IDENTITY_RESOLVED' >/dev/null
printf '%s\n' "$connection_callback" | grep -F \
    'BLE_GAP_EVENT_REPEAT_PAIRING' >/dev/null
printf '%s\n' "$connection_callback" | grep -F \
    'BLE_GAP_EVENT_PASSKEY_ACTION' >/dev/null
printf '%s\n' "$connection_callback" | grep -F \
    '_ble_nimble_port_gap_event(event, arg)' >/dev/null

echo "BLE GAP connection callback routing verified"

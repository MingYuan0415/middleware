#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
port_source="$script_dir/../src/ble_nimble_port.c"
service_source="$script_dir/../src/ble_link_service.c"

consumer=$(awk '
    /static void _ble_nimble_port_link_gatt_consumer\(/ { in_function = 1 }
    in_function { print }
    in_function && /^}/ { exit }
' "$port_source")

printf '%s\n' "$consumer" | grep -F 'BLE_PORT_EVENT_SUBSCRIBE' >/dev/null
printf '%s\n' "$consumer" | grep -F \
    'ble_link_gatt_session_tx_handle()' >/dev/null
printf '%s\n' "$consumer" | grep -F \
    'ble_link_service_set_transport_admitted(' >/dev/null
grep -F 'if (!s_service.transport_admitted)' "$service_source" >/dev/null

echo "Queued work transport admission routing verified"

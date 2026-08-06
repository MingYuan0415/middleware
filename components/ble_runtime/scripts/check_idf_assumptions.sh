#!/bin/sh

set -eu

if [ -z "${IDF_PATH:-}" ]; then
    echo "IDF_PATH is required" >&2
    exit 1
fi

version="$($IDF_PATH/tools/idf.py --version)"
if [ "$version" != "ESP-IDF v6.0.2" ]; then
    echo "ESP-IDF baseline changed: expected ESP-IDF v6.0.2, got $version" >&2
    exit 1
fi

check_source()
{
    relative_path="$1"
    expected_hash="$2"
    source_path="$IDF_PATH/$relative_path"
    if [ ! -f "$source_path" ]; then
        echo "ESP-IDF source missing: $relative_path" >&2
        exit 1
    fi
    actual_hash="$(sha256sum "$source_path" | awk '{print $1}')"
    if [ "$actual_hash" != "$expected_hash" ]; then
        echo "ESP-IDF source changed; manual BLE runtime review required: $relative_path" >&2
        exit 1
    fi
}

NIMBLE="components/bt/host/nimble/nimble"
NIMBLE_HOST="$NIMBLE/nimble/host"

check_source \
    "$NIMBLE_HOST/include/host/ble_gatt.h" \
    "bb321c6e8d7c1b4245e04f9a927e443368a46495139ed15d26f93b2c8511aff0"
check_source \
    "$NIMBLE_HOST/include/host/ble_gap.h" \
    "f98ffc8c997ca99f0664af90d28bdd34b21a9c867c9231e73270fa0b8477e2e3"
check_source \
    "$NIMBLE_HOST/src/ble_gap.c" \
    "fb2ba887898dae3ec25376e244b0e13694b179f86f509579752925c6cf1feacc"
check_source \
    "$NIMBLE_HOST/src/ble_gatts.c" \
    "6ecefb0b156c95b6d397dad2121da69f35883c0b42bb1a2435f524e4abf5fa6d"
check_source \
    "$NIMBLE/porting/nimble/src/nimble_port.c" \
    "ca884c8d4dd17a248732219ca53572526d3b31a38ff28a9aa92a349ca3d7e7ec"
check_source \
    "$NIMBLE_HOST/src/ble_hs.c" \
    "2b9626667d4601dff6e7c7bfdb37edf17567ac7da7d78a0d1d76983aac2bce35"
check_source \
    "$NIMBLE_HOST/src/ble_hs_mbuf.c" \
    "a4c4caec4990adb35c1fc3b3a16d13674988ec040b4b930e428d6ea87e2255a8"
check_source \
    "$NIMBLE_HOST/include/host/ble_att.h" \
    "a97eca1a502bbf4d1f91a0ce6ff0996c74dc68606e13e8e67f7fcfd676e6c9c7"

echo "ESP-IDF BLE runtime assumptions verified"

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
    "$NIMBLE_HOST/src/ble_hs_pvcy.c" \
    "bf5c4650e974e51a8055cf2b6ca278a81f9c47ccb9606445de28d6a43ae46756"
check_source \
    "$NIMBLE_HOST/src/ble_store.c" \
    "901b3671751f9d381c717adf4ba213d9a9b87a6162d33195f7cbd6fbd724a6ca"
check_source \
    "$NIMBLE_HOST/src/ble_hs_conn.c" \
    "a502dda47f92e14a1b084ea58f5e8235c31799b5549ffb0e1df8c5f506990c0b"
check_source \
    "$NIMBLE_HOST/src/ble_store_util.c" \
    "c66e4bdde4c8cac5b47401dfe8404908ac2706b4046c740dbfe04d7c00a97fa5"
check_source \
    "$NIMBLE_HOST/store/config/src/ble_store_config.c" \
    "88257faee279574ed4651bdb95e73c4d16ed6137477af21b4c3cab736c0f24d7"
check_source \
    "$NIMBLE_HOST/store/config/src/ble_store_config_conf.c" \
    "1b0b79c4fc784dbed662f95e41d181b75d1716ded4ffab269e4f74403680c569"
check_source \
    "$NIMBLE_HOST/src/ble_gatts.c" \
    "6ecefb0b156c95b6d397dad2121da69f35883c0b42bb1a2435f524e4abf5fa6d"
check_source \
    "$NIMBLE_HOST/src/ble_gattc.c" \
    "6b77f7726413539972404ea1de6788c06950fac8654d564443dcb064b0e7f351"
check_source \
    "$NIMBLE_HOST/src/ble_att_svr.c" \
    "6a0c33d7c11a81fd022c13349a3da8b8da82993c41177568a8a5238cbf2c0799"
check_source \
    "$NIMBLE_HOST/src/ble_sm.c" \
    "384e25aae3ea1f623aef7a6f10cefe5ed7bbeb972f2ee163a6c6ed3ba2e01a7e"
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
check_source \
    "components/bt/host/nimble/port/include/esp_nimble_cfg.h" \
    "ec558db1eed63c71d5cd056d54943ab2cd20cae5188925486284d546469a5727"

echo "ESP-IDF BLE runtime assumptions verified"

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

check_contains()
{
    relative_path="$1"
    expected_text="$2"
    source_path="$IDF_PATH/$relative_path"
    if ! grep -F "$expected_text" "$source_path" >/dev/null; then
        echo "ESP-IDF assumption missing in $relative_path: $expected_text" >&2
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
    "$NIMBLE_HOST/store/config/src/ble_store_nvs.c" \
    "8ae9aac14729466943d0604d5c7602f4482ba5967b17b083dbd1666d04a1a538"
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
    "$NIMBLE_HOST/src/ble_hs_cfg.c" \
    "5d0dedb23d5e7c1ca7512d95fd81bc2804c52cc51e291f0ed9903376c4163f3b"
check_source \
    "$NIMBLE_HOST/src/ble_hs_startup.c" \
    "e9e522760447054d4134137f4ae5e269a1a2b825616507ef994834869c9c1ad1"
check_source \
    "$NIMBLE_HOST/src/ble_hs_mbuf.c" \
    "a4c4caec4990adb35c1fc3b3a16d13674988ec040b4b930e428d6ea87e2255a8"
check_source \
    "$NIMBLE_HOST/include/host/ble_att.h" \
    "a97eca1a502bbf4d1f91a0ce6ff0996c74dc68606e13e8e67f7fcfd676e6c9c7"
check_source \
    "components/bt/host/nimble/port/include/esp_nimble_cfg.h" \
    "ec558db1eed63c71d5cd056d54943ab2cd20cae5188925486284d546469a5727"
check_source \
    "components/bt/host/nimble/Kconfig.in" \
    "a1e0cf4df22d06697f359ed8752aab525d694a0eb0b6b25917e73196afa11876"

# The cold-boot host config has no store writer. NimBLE privacy startup installs
# one during every controller startup. ble_hs_sync() runs this path
# both initially and after a host reset, before invoking the project sync
# callback that captures or restores its guard.
check_contains \
    "$NIMBLE/porting/nimble/src/nimble_port.c" \
    "ble_transport_hs_init();"
check_contains \
    "$NIMBLE_HOST/src/ble_hs.c" \
    "rc = ble_hs_startup_go();"
check_contains \
    "$NIMBLE_HOST/src/ble_hs.c" \
    "rc = ble_hs_sync();"
check_contains \
    "$NIMBLE_HOST/src/ble_hs.c" \
    "    ble_hs_sync();"
check_contains \
    "$NIMBLE_HOST/src/ble_hs.c" \
    "ble_hs_cfg.sync_cb();"
check_contains \
    "$NIMBLE_HOST/src/ble_hs_startup.c" \
    "ble_hs_pvcy_set_default_irk();"
check_contains \
    "$NIMBLE_HOST/src/ble_hs_pvcy.c" \
    "ble_store_config_init();"
check_contains \
    "$NIMBLE_HOST/store/config/src/ble_store_config.c" \
    "ble_hs_cfg.store_write_cb = ble_store_config_write;"
check_contains \
    "$NIMBLE_HOST/src/ble_hs_pvcy.c" \
    "ble_hs_pvcy_remove_entry(uint8_t addr_type, const uint8_t *addr)"

# ESP32-S3 uses controller privacy, not ESP32-only host privacy. Therefore the
# persisted schema contains the six public store families below and no private
# p_dev_rec family. The compiled NVS implementation discards restore errors;
# the runtime's pre-reconciliation audit is the fail-closed boundary.
check_contains \
    "components/bt/host/nimble/Kconfig.in" \
    "depends on BT_NIMBLE_ENABLED && BT_NIMBLE_HS_PVCY && IDF_TARGET_ESP32"
check_contains \
    "components/bt/host/nimble/port/include/esp_nimble_cfg.h" \
    "#define MYNEWT_VAL_BLE_HOST_BASED_PRIVACY (0)"
check_contains \
    "$NIMBLE_HOST/store/config/src/ble_store_nvs.c" \
    "#define NIMBLE_NVS_NAMESPACE                     \"nimble_bond\""
check_contains \
    "$NIMBLE_HOST/store/config/src/ble_store_nvs.c" \
    "err = ble_nvs_restore_sec_keys();"
check_contains \
    "$NIMBLE_HOST/store/config/src/ble_store_nvs.c" \
    "ESP_LOGE(TAG, \"NVS operation failed, can't retrieve the bonding info\");"

# Generic PEER_ADDR iteration seeds BLE_ADDR_ANY, but the fixed config-store
# finder compares that address exactly against both fields. It therefore
# cannot enumerate RPA_REC and the runtime must use durable blobs plus exact
# ble_store_read_rpa_rec() keys for restore, cleanup and reset verification.
check_contains \
    "$NIMBLE_HOST/src/ble_store.c" \
    "key.rpa_rec.peer_rpa_addr = *BLE_ADDR_ANY;"
check_contains \
    "$NIMBLE_HOST/store/config/src/ble_store_config.c" \
    "ble_addr_cmp(&rpa_rec->peer_rpa_addr, &key->peer_rpa_addr) &&"
check_contains \
    "$NIMBLE_HOST/store/config/src/ble_store_config.c" \
    "ble_addr_cmp(&rpa_rec->peer_addr, &key->peer_rpa_addr)"

echo "ESP-IDF BLE runtime baseline assumptions verified"

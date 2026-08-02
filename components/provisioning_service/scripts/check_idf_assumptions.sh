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
        echo "ESP-IDF source changed; manual provisioning review required: $relative_path" >&2
        exit 1
    fi
}

check_source \
    "components/protocomm/src/transports/protocomm_nimble.c" \
    "3647ad63c212b212378780db4cfc5288f68028f646fd6d6cdedb7a2ed65785b9"
check_source \
    "components/protocomm/src/crypto/srp6a/esp_srp.c" \
    "7ab3085f214250c5dacd6003fddbd3842b3d1c884f2d67905d693987a2f67545"

echo "ESP-IDF provisioning assumptions verified"

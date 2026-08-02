#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
component_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
project_root=$(CDPATH= cd -- "$component_dir/../../../.." && pwd)
contract_dir="$project_root/contracts/provisioning"
expected_commit=8d3eac830c3901ddf00fc3b092cbc90d2618bf96
generator=${PROTOC_C:-protoc-c}

actual_commit=$(git -C "$contract_dir" rev-parse HEAD)
if [ "$actual_commit" != "$expected_commit" ]; then
    echo "contract commit mismatch: expected $expected_commit, got $actual_commit" >&2
    exit 1
fi

versions=$($generator --version)
generator_version=$(printf '%s\n' "$versions" | sed -n '1p')
compiler_version=$(printf '%s\n' "$versions" | sed -n '2p')
if [ "$generator_version" != "protobuf-c 1.4.1" ]; then
    echo "protoc-c version mismatch: expected protobuf-c 1.4.1, got $generator_version" >&2
    exit 1
fi
if [ "$compiler_version" != "libprotoc 3.21.12" ]; then
    echo "libprotoc version mismatch: expected libprotoc 3.21.12, got $compiler_version" >&2
    exit 1
fi

temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

$generator --c_out="$temporary" -I"$contract_dir/proto" \
    "$contract_dir/proto/microtech/provisioning/v1/types.proto" \
    "$contract_dir/proto/microtech/provisioning/v1/provisioning.proto"

for generated in types.pb-c.c types.pb-c.h provisioning.pb-c.c provisioning.pb-c.h; do
    committed="$component_dir/src/generated/microtech/provisioning/v1/$generated"
    candidate="$temporary/microtech/provisioning/v1/$generated"
    if ! cmp -s "$committed" "$candidate"; then
        echo "generated protobuf differs: $generated" >&2
        diff -u "$committed" "$candidate" || true
        exit 1
    fi
done

echo "provisioning protobuf generation verified"

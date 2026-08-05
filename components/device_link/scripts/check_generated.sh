#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
component_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
project_root=$(CDPATH= cd -- "$component_dir/../../../.." && pwd)
contract_dir="$project_root/contracts/provisioning"
lock_file="$component_dir/proto.lock"
generator=${PROTOC_C:-protoc-c}

expected_commit=$(sed -n 's/^contract_commit=//p' "$lock_file")
expected_compiler=$(sed -n 's/^protoc_version=//p' "$lock_file")
expected_generator=$(sed -n 's/^protoc_gen_c_version=//p' "$lock_file")

actual_commit=$(git -C "$contract_dir" rev-parse HEAD)
if [ "$actual_commit" != "$expected_commit" ]; then
    echo "contract commit mismatch: expected $expected_commit, got $actual_commit" >&2
    echo "update $lock_file only after the contract commit is immutable" >&2
    exit 1
fi
if [ -n "$(git -C "$contract_dir" status --porcelain)" ]; then
    echo "contract worktree is dirty; generation must come from a clean commit" >&2
    exit 1
fi

versions=$($generator --version)
generator_version=$(printf '%s\n' "$versions" | sed -n '1p')
compiler_version=$(printf '%s\n' "$versions" | sed -n '2p')
if [ "$generator_version" != "protobuf-c $expected_generator" ]; then
    echo "protoc-c version mismatch: expected protobuf-c $expected_generator, got $generator_version" >&2
    exit 1
fi
if [ "$compiler_version" != "libprotoc $expected_compiler" ]; then
    echo "libprotoc version mismatch: expected libprotoc $expected_compiler, got $compiler_version" >&2
    exit 1
fi

temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

$generator --c_out="$temporary" -I"$contract_dir/proto" \
    "$contract_dir/proto/microtech/link/v1/errors.proto" \
    "$contract_dir/proto/microtech/link/v1/capabilities.proto" \
    "$contract_dir/proto/microtech/link/v1/session.proto" \
    "$contract_dir/proto/microtech/link/v1/events.proto" \
    "$contract_dir/proto/microtech/link/v1/transfer.proto" \
    "$contract_dir/proto/microtech/link/v1/envelope.proto"

for generated in errors capabilities session events transfer envelope; do
    for suffix in pb-c.c pb-c.h; do
        committed="$component_dir/src/generated/microtech/link/v1/$generated.$suffix"
        candidate="$temporary/microtech/link/v1/$generated.$suffix"
        if ! cmp -s "$committed" "$candidate"; then
            echo "generated protobuf differs: $generated.$suffix" >&2
            diff -u "$committed" "$candidate" || true
            exit 1
        fi
    done
done

echo "device link protobuf generation verified"

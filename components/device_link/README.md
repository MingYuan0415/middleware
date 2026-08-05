# device_link

MicroTech Device Link BLE service primitives.

## Framing

`device_link_framing` implements the Device Link v1 fragment contract
(`contracts/provisioning/docs/device-link-framing-v1.md`): an eight-byte header
plus payload inside each GATT value, one reassembly slot per connection
generation and channel, idempotent duplicate acceptance, and strict rejection
of gaps, overlaps, unknown flags, and out-of-order starts. Fragment payload
limits follow the ATT MTU boundaries (12/174/487 bytes at MTU 23/185/498).

The module is dependency-free and host-testable; it does not own NimBLE,
Protocomm, or transport state. Session and control admission belong to the
runtime layers that use it. The experimental Security 2 transport prototype
from P0 is retained under `probe/` for evidence only and is not built.

## Pinned protocol consumer

`src/generated/` holds protobuf-c sources generated from the pinned contract
commit in `proto.lock`. `scripts/check_generated.sh` re-generates from the
contract worktree and fails when the contract commit does not match the lock
or the worktree is dirty, so generation is reproducible from clean commits.

## Host tests

```sh
cmake -S tests/host -B /tmp/mt-device-link -G Ninja \
  && cmake --build /tmp/mt-device-link \
  && ctest --test-dir /tmp/mt-device-link --output-on-failure
```

Sanitizer variants: `-DDEVICE_LINK_SANITIZER=address|thread` (default none).

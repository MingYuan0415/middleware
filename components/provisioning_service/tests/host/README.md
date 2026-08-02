# Provisioning host tests

This suite links the generated protobuf-c contract consumer, production request
processor, and provisioning worker against fake connectivity, Protocomm BLE,
event bus, and FreeRTOS ports. It covers malformed wire rejection and request
scrubbing, polling operations, transport lifecycle, cancellation, retained
terminal state, disabled events, secret renewal, timeout, and finish ordering.

Run from the repository root:

```sh
cmake -S layers/middleware/components/provisioning_service/tests/host \
    -B /tmp/mt-provisioning -G Ninja
cmake --build /tmp/mt-provisioning
ctest --test-dir /tmp/mt-provisioning --output-on-failure
```

Set `-DPROVISIONING_SANITIZER=address` for ASan/UBSan or
`-DPROVISIONING_SANITIZER=thread` for TSan.

# ble_runtime

MicroTech BLE runtime, the single owner of the NimBLE host lifecycle and
`ble_hs_cfg`. Managers for GAP, advertising, the static GATT registry, power,
and metrics are added on top of this lifecycle; the NimBLE adapter is the only
module allowed to call `nimble_port_*` or write `ble_hs_cfg`.

The lifecycle itself is host-testable through the injected `ble_runtime_host_port_t`
backend. See `tests/host/` for the state machine matrix, failure rollback, and
repeated-call rejection tests.

```sh
cmake -S tests/host -B /tmp/mt-ble-runtime -G Ninja \
    -DBLE_RUNTIME_SANITIZER=none
cmake --build /tmp/mt-ble-runtime
ctest --test-dir /tmp/mt-ble-runtime --output-on-failure
```

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

## NimBLE adapter fault model

The adapter owns the host task itself (created with the project NimBLE stack
and core configuration) so task-creation failures are reported instead of
silently swallowed. A failed `nimble_port_deinit()` is a terminal fault: the
adapter latches the error, every later retry returns the same error, and only
a device reboot can recover the controller/host resources. Sync timeouts are
bounded and retryable.

The stop path calls `nimble_port_stop()`, whose internal completion wait is
unbounded in ESP-IDF: it returns promptly while the host event loop is
healthy, but if the host task is wedged or never synchronized, the stop call
blocks permanently. That condition is treated as a terminal fault too; only a
device reboot recovers it.

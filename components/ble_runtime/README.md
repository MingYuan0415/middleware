# ble_runtime

MicroTech BLE runtime, the single owner of the NimBLE host lifecycle and
`ble_hs_cfg`. Managers for GAP, advertising, the static GATT registry, power,
and metrics are added on top of this lifecycle; the NimBLE adapter is the only
module allowed to call `nimble_port_*` or write `ble_hs_cfg`.

The lifecycle itself is host-testable through the injected
`ble_runtime_host_port_t` backend. See `tests/host/` for the state machine
matrix, failure rollback, and repeated-call rejection tests.

```sh
# from a shell with IDF_PATH exported (source <esp-idf>/export.sh first)
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
a device reboot can recover the controller/host resources.

The stop path runs `nimble_port_stop()` on a dedicated worker task and waits on
a bounded timeout, so a wedged or never-synchronized host turns into a bounded
`ESP_ERR_TIMEOUT` instead of a permanent block. That condition latches the same
terminal-fault state: subsequent calls return the latched error and only a
device reboot recovers it. Sync timeouts are likewise bounded and retryable.
`nimble_port_deinit()` is only invoked when `nimble_port_init()` actually
succeeded; a failed or partially-rolled-back init never tears down an
uninitialized host. When a terminal fault is latched, the retained resources
are intentionally kept (to avoid use-after-free) until the reboot that alone
recovers the fault; a teardown retry releases them only when no fault is
latched.

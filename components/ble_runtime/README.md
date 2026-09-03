# ble_runtime

`ble_runtime` is the sole owner of the ESP-IDF NimBLE host lifecycle and
`ble_hs_cfg`. It provides GAP admission, advertising, the static GATT database,
Device Link v1 transport, Numeric Comparison, and persistent bond-store
coordination. Other components must use its public APIs rather than calling
`nimble_port_*` or changing `ble_hs_cfg` directly.

The `device_link_service` task owns product policy: the 120-second local pairing
window, user confirmation commands, status events, Bluetooth enablement, and
local bond revoke. NimBLE callbacks retain connection-qualified facts and wake
that owner; they do not update LVGL or publish application events directly.

## Host Lifecycle

`ble_runtime_host_port_t` injects init/start, pairing-gate synchronization,
peer-store reset, stop, and deinit. Host synchronization audits restored NVS
records before advertising or pairing is enabled. Stop runs on a dedicated
worker with bounded acknowledgement; a timeout or terminal deinit error remains
latched until reboot so resources cannot be freed under a live host task.

The `ble_link_timer` task owns the indication deadline, retained peer cleanup,
local revoke, and accepted/rejected ACL termination. Absolute obligations are
stored outside its command queue, so a dropped wake hint cannot discard work.
Clean shutdown closes admission and advertising, crosses a host-queue barrier,
drains all retained obligations, then crosses a second barrier before exit.

## Connection Identity

Asynchronous work carries `ble_link_operation_identity_t`:

```text
{generation, security_epoch, flow_id, token, kind, conn_handle}
```

Disconnect, reset, MTU, encryption, subscription, TX completion, termination,
and cleanup paths validate the applicable fields. A stale generation or reused
connection handle cannot retire a newer ACL. The CONNECT and final ENC_CHANGE
paths both read the current NimBLE descriptor so identity resolution and
encryption events converge regardless of callback order.

The security reducer distinguishes a restored bond from a bond created by the
current ACL. A new pairing becomes an uncommitted candidate when Numeric
Comparison starts. A verified Secure Connections, MITM, 16-byte bond is the v1
durable binding boundary; after authorization succeeds it is committed and is
kept across disconnect. Cancellation, reset, or disconnect retains cleanup only
for a fresh pairing that started but never reached that boundary. Malformed or
window-ineligible bonds are deleted and their ACL is terminated.

## Device Link v1 GATT

The database contains one service and two characteristics:

- `command_rx`: authenticated Write Request.
- `server_tx`: authenticated Indicate with CCCD value `02 00`.

There is no application-layer Security 2 handshake and no legacy
`link_state`, `control_rx`, or `control_tx` characteristic in v1. Link
authentication is provided solely by the bonded Secure Connections transport.

The preferred and required ATT MTU is 498, yielding a 495-byte characteristic
value. Each Write contains one complete v1 frame; there is no application-layer
fragment reassembly. The route gate checks encryption, verified SC/MITM bond,
CCCD state, size, and the single outstanding indication before dispatching
application work. The owner repeats the current connection and `server_tx`
CCCD check immediately before queued work executes, so disabling indications
cannot start a Wi-Fi operation whose result has no return path. ATT security
errors remain visible to the client so it can initiate pairing.

The TX scheduler permits one indication in flight. A two-second absolute
deadline retires the exact operation on timeout while keeping a raw-callback
tombstone until a late NimBLE completion or ACL teardown consumes it. This
prevents a completion without an application token from confirming a newer
indication that reused the same handle tuple.

## Numeric Comparison

NimBLE is configured for DisplayYesNo, bonding, MITM, Secure Connections only,
16-byte keys, and ENC plus identity key distribution. Only
`BLE_SM_IOACT_NUMCMP` is accepted. The port records the current ACL handle and
an epoch, obtains a service token, and wakes the Device Link owner without
logging the six-digit value.

Local accept/reject validates the exact token and calls `ble_sm_inject_io()`.
Success consumes the pending action; failure restores it only if the handle and
epoch are unchanged. Disconnect, reset, window close, unsupported passkey
action, and SMP failure clear the local pending state and wake the owner so the
Setup page hides or restores its controls from a new status generation.

Repeated pairing never replaces a verified stored bond. Retry is allowed only
for incomplete material when the device is bindable and no durable bond exists.
Binding replacement therefore requires the local revoke flow.

## Advertising

The manager reduces slow non-bindable and fast bindable leases to one target.
`bindable` is internal control state: a transition restarts advertising and
synchronizes the pairing gate even though it is not serialized on air. The
encoder emits only Flags, the complete 128-bit Device Link service UUID, and an
optional shortened local name. It rejects payloads above the 31-byte legacy
advertising limit before calling NimBLE.

Production uses `BLE_OWN_ADDR_PUBLIC`. It does not publish Service Data,
discriminator, instance ID, or address-rotation metadata.

START and STOP operations carry nonzero generations. Only the completion for
the current transition may mutate manager state; stale completions are no-ops.
Failures retain the exact action and generation with bounded exponential retry,
and the owner waits on the nearest absolute deadline rather than periodic
polling.

## Pairing Gate And Store

An unbonded peer may pair only while the local window is open. A verified bonded
peer may reconnect outside the window. Pairing-gate holds cover cleanup,
rejected ACL termination, drain, and revoke; effective pairing is enabled only
when the window is requested and no hold remains. GAP admission is closed while
mandatory cleanup is retained.

Peer cleanup verifies removal of OUR_SEC, PEER_SEC, CCCD, and applicable privacy
records. Store write and restore errors are sticky for the current host run;
absence from the RAM mirror cannot retire a cleanup after persistence failed.
Local revoke and factory reset use a journaled full-store reset and reopen
advertising only after durable NVS and RAM mirrors are both empty. The journal
is `ble_nimble_port_revoke_journal` (versioned `ble.revoke` blob in
`nv_storage`): `begin` persists the intent, `pending` fails closed on a
malformed blob, and `end` clears it only after the verified-empty sweep. A
revoke request only queues host-core work; Device Link observes journal absence
before declaring completion and retries while the marker remains. `begin`
rewrites malformed markers, including blobs larger than the current version.

The root build pins the required ESP-IDF v6.0.2 behavior, including
`MYNEWT_VAL_BLE_RESTART_PAIR=0`. `scripts/check_idf_assumptions.sh` verifies the
GAP, GATT, ATT, SM, store, and host-event source assumptions used here.

## Host Tests

```sh
cmake -S tests/host -B /tmp/mt-ble-runtime -G Ninja \
    -DBLE_RUNTIME_SANITIZER=none
cmake --build /tmp/mt-ble-runtime
ctest --test-dir /tmp/mt-ble-runtime --output-on-failure
```

`BLE_RUNTIME_SANITIZER` also accepts `address` and `thread`. The suite covers
lifecycle, GATT registry and gate behavior, GAP admission, advertising payload
and convergence, TX credit/completion/tombstones, indication deadlines, cleanup
capacity and fences, Numeric Comparison policy, sticky store guards, session
security state, and pinned IDF assumptions. On x86_64, CTest disables ASLR for
TSan when `setarch` is available.

Host tests do not prove ESP32-S3 NimBLE callback timing, NVS persistence, radio
behavior, reset/cold-start behavior, or interoperability with Android.

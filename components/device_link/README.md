# Device Link Typed-TLV v2

`device_link` is the handwritten application-wire implementation for Device
Link Core v2. It is deliberately independent of NimBLE, Protocomm and the
ESP-IDF Security 2 implementation.

## Wire layers

The component owns a fixed 16-byte application header, bounded BLE fragment
framing, and a strict Typed-TLV payload codec. The codec rejects truncated
fields, non-minimal integers, duplicate singular fields, invalid ordering and
unknown values that are unsafe to interpret. Unknown fields with a registered
wire type may be skipped, so minor additions do not require a firmware rebuild.

The router uses startup-frozen descriptors. Each method binds a domain and
version, request/response schema, permission, payload limits and owner handler;
there is no runtime registration or reflection. Core is domain `0`; Wi-Fi,
Cloud and Location use reserved IDs `1`, `2` and `3`. Core is always
registered; the Wi-Fi domain is registered conditionally (explicit
DEVICE_LINK_SERVICE_WIFI_ADVERTISED capability gate; the compile-time gate is
the only publish decision) and
is currently not advertised in production builds. Domain adapters must be
complete and validated before a capability is published.

Replay protection uses fixed storage. A replay key includes boot ID, connection
generation, Security 2 epoch, domain, method and call ID plus request length
and digest. Responses are retained without sensitive request bytes. Operation
state uses four statically allocated slots; no `malloc` or `realloc` is used on
the protocol path. Sensitive buffers are cleared on completion, failure and
disconnect.

The component does not contain application protobuf schemas or generated C.
The immutable contract pin is recorded in `device-link-contract.lock`; it contains the
Device Link contract commit, profile, schema format, and normalized schema
digest. It intentionally has no `protoc` or code-generator version because
the application wire is handwritten Typed-TLV.
expectedContractCommit=fa8b314c47df84d2cbfa257813086004e3db0d58
The only protobuf-c types in the middleware are the official ESP-IDF
Protocomm Security 2 messages in `device_link_security`.

Schema boundaries mirror the contract: `AuthorizePrepareResponse.expires_in_ms`
is frozen in `[1, 120000]` and permission list entries are nonzero
(`minimum_unsigned = 1`), both enforced by the TLV validator. The
`ble_runtime` host suite consumes the contract fixture goldens
(wire/framing/authorization/error-responses/operation-result bodies) in
CTest; the `device_link_service` host suite consumes the Wi-Fi invalid
credential cases and the `WifiStatus` result payloads.

## Host tests

```sh
cmake -S tests/host -B /tmp/mt-device-link -G Ninja \
  -DDEVICE_LINK_SANITIZER=none
cmake --build /tmp/mt-device-link
ctest --test-dir /tmp/mt-device-link --output-on-failure
```

Use `-DDEVICE_LINK_SANITIZER=address` or `thread` in an independent build
directory. Canonical YAML schemas and fixtures live in the contract submodule;
this component keeps the corresponding C descriptors and golden byte tests.

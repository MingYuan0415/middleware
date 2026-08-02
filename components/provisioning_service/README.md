# Provisioning service

This component implements the firmware consumer of
`contracts/provisioning` at commit
`8d3eac830c3901ddf00fc3b092cbc90d2618bf96`.

The first release exposes only the polling workflow over ESP Protocomm BLE
Security 2 patch 1. It does not advertise encrypted events. Wi-Fi policy and
credential persistence remain owned by `connectivity_manager`.

Generated protobuf-c sources are committed here because the firmware consumer
owns its generated code. Regenerate and verify them with protobuf-c 1.4.1:

```sh
PROTOC_C=protoc-c \
    components/provisioning_service/scripts/check_generated.sh
```

The service uses the ESP-IDF 6.0.2 built-in Protocomm implementation. Its
Protocomm component is compiled with `LOG_LOCAL_LEVEL=ESP_LOG_INFO`, so the
upstream Security 2 debug hexdump is excluded from this firmware.

Application-owned plaintext requests, POP, QR data, salt, verifier, and
temporary Wi-Fi credential copies are overwritten before release. ESP-IDF's
internal Security 2 SRP/session heap is still released without guaranteed
explicit zeroization. Strict secret-zeroization conformance therefore remains
blocked on an upstream fix and must not be reported as passed.

If `protocomm_ble_stop()` fails, ESP-IDF has already consumed its internal
transport binding. The service fails closed, releases only project-safe
objects, clears application secrets, keeps standby blocked, and rejects reopen
and successful deinitialization for the rest of that boot. Reboot is the only
supported recovery.

The reviewed ESP-IDF version and source assumptions are checked without
copying or patching upstream code:

```sh
components/provisioning_service/scripts/check_idf_assumptions.sh
```

Any change to the reviewed NimBLE stop or SRP cleanup sources requires a manual
Security 2 and transport-lifecycle review before updating the expected hashes.

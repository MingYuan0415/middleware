# Security 2 transport probe (experimental, not built)

This directory retains the development prototype that owned NimBLE and ran a
custom GATT transport against Protocomm Security 2. It proved the transport
concept during P0 but does not satisfy the Device Link contract: no Secure
Connections bond admission, no second-ACL rejection, unlimited indication
waits, and no protocol-timeout semantics. It is deliberately excluded from
the `device_link` component build and from the firmware dependency graph; the
P1 `ble_runtime` replaces it, and this directory is kept only as evidence.

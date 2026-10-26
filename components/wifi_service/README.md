**English** | [简体中文](README_ZH.md)

# Wi-Fi Service

`wifi_service` is the worker-owned single-radio executor. Commands deep-copy
credentials and use nonzero session and operation identities. Radio policy and
durable profile selection belong to its caller, not the driver boundary.

## Operation results and link facts

The status snapshot's `operation_complete`, `operation_result`,
`operation_failure`, and `operation_canceled` describe the CONNECT/DISCONNECT
identified by `session_id` and `operation_id`. A completed result remains sticky
until the next CONNECT/DISCONNECT adoption or full initialization. Scan snapshots
have their own identity and result; scans and IPv4 updates cannot overwrite a
status terminal. Releasing executor ownership is distinct from completing it:
successful suspension releases an unfinished CONNECT without manufacturing a
terminal success or cancellation.

`state`, `failure`, `last_error`, `ipv4_address`, and `desired_connected` describe
current link facts, not an operation's outcome. A replacement CONNECT cannot
report an old association's IP as its success. Same-epoch GOT_IP updates after
IP_READY, including connected scanning, update only the address; duplicate
addresses do not publish another snapshot or complete another CONNECT.

## Radio boundaries and secrets

Radio transitions stop and drain the executor, verify STOPPED, clear driver
credentials, and optionally start it again. Configuration is never cleared while
association is active. Every stage preserves the first error and observes the
actual final port state. A fully stopped, cleared suspend commits SUSPENDED even
when stop returned an error, while the control call still returns that error.
Unusable partial states retain cleanup obligations and publish OFFLINE with IP0.
Worker credentials are wiped when an attempt is abandoned or suspended.

The ESP-IDF port retains initialized resources if configuration clearing fails
during deinit, so a later deinit can retry rather than hiding a failed erase.

## Verification boundary

`tests/connectivity` links the real executor and manager with a controlled radio
boundary, and separately compiles the real ESP-IDF port against an SDK fake.
These tests do not establish radio timing, DHCP interoperability, SDK callback
coverage, or NVS power-loss behavior. Device Link wire framing remains defined in
`contracts/device_link/protocol.yaml`.

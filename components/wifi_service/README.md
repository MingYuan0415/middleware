# Wi-Fi Service Device Link Policy

## Current status

This document is non-normative for the Device Link BLE wire contract. The
current `wifi_service` implementation remains the single-radio asynchronous
executor used by `connectivity_manager`. Existing behavior predates the Device
Link v1 candidate and may overlap with individual items below. The component is
not currently declared conformant with the complete target.

## Pending policy target

Status: pending implementation.

The following Wi-Fi implementation decisions are pending:

- persist credentials without starting a connection, and expose them to a
  later explicit connect request;
- define durable-profile replacement and storage-failure rollback behavior;
- define connection retry limits, delays, timeouts, and automatic connection;
- define how an explicit disconnect interacts with later automatic connection;
- define disconnect-and-erase ordering and failure recovery for forget;
- select, sanitize, deduplicate, order, and truncate raw scan results before
  publishing at most five wire records;
- define how radio, storage, lifecycle, and admission failures are classified
  for the Device Link status and Wi-Fi failure values exposed by the adapter.

Until the complete target is implemented and tested, this component does not
claim Device Link v1 policy conformance. The normative BLE framing, Wi-Fi wire
messages, observable command results, and operation recovery contract remain in
`contracts/device_link/protocol.yaml`.

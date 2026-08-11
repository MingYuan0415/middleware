# Factory Reset Service

`factory_reset_service` owns the durable, versioned factory-reset journal. It
does not erase product data itself. `request()` first commits the journal and
only then invokes the injected restart callback; a storage failure never
restarts the device. Journal operations are serialized through the restart
callback; that callback must not re-enter this service.

## Recovery contract

Startup treats a valid `factory.reset` marker as a fail-closed obligation. The
root runtime clears each reset domain idempotently before it permits network or
advertising startup:

1. clear the persisted Wi-Fi profile before Connectivity Manager init;
2. start Device Link in `FACTORY_RESET_GATED` mode so authorization, verifier,
   bond/CCCD, and volatile transfer state converge while advertising is paused;
3. acquire the persistent slow, non-bindable advertising lease while the
   marker is still durable and advertising remains paused;
4. clear the global marker only after those reset domains and advertising
   prerequisites report success;
5. release the Device Link startup gate, then continue platform and network
   startup.

Gate release is only the visibility commit that unpauses advertising. A
physical advertising-start failure is retained by the ADV owner's bounded
retry state, so it does not reopen the completed storage transaction or permit
network startup to race an unprepared Device Link service.

`recovery_pending()` distinguishes an absent marker from storage and integrity
errors. A truncated, corrupt, or unreadable marker is never treated as “no
reset pending.” `complete_recovery()` is idempotent; an erase or erase-commit
failure leaves the previously committed marker recoverable after a power
cycle.

## Host tests

The host fake distinguishes staged and committed NVS data and provides a
deterministic power-cycle operation. Tests cover set/get/erase failures, commit
failures on write and erase, corrupt and truncated markers, concurrent request
admission, concurrent erase/read exclusion, deinit exclusion through the
restart callback, and recovery after power loss. Device Link host tests
separately inject the post-marker physical advertising-start failure and verify
automatic retry from the pre-acquired lease.

```sh
cmake -S components/factory_reset_service/tests/host \
    -B /tmp/mt-factory-reset -G Ninja \
    -DFACTORY_RESET_SERVICE_SANITIZER=none
cmake --build /tmp/mt-factory-reset
ctest --test-dir /tmp/mt-factory-reset --output-on-failure
```

The sanitizer option also accepts `address` and `thread`. Host tests do not
replace ESP32-S3 NVS, restart, brownout, or cold-cycle validation.

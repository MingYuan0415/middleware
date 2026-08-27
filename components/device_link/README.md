# Device Link v1

`device_link` is the fixed-binary codec and single-slot operation engine for
`device-link/v1`. GATT, SMP, and Wi-Fi submission live in `ble_runtime` and
`device_link_service`.

The immutable contract pin is `device-link-contract.lock`.

## Host tests

```sh
cmake -S tests/host -B /tmp/mt-device-link -G Ninja \
  -DDEVICE_LINK_SANITIZER=none
cmake --build /tmp/mt-device-link
ctest --test-dir /tmp/mt-device-link --output-on-failure
```

Use `-DDEVICE_LINK_SANITIZER=address` or `thread` in an independent build
directory.

[English](README.md) | **简体中文**

# Device Link v1

`device_link` 是 `device-link/v1` 的固定二进制编解码器与单槽操作引擎。GATT、
SMP 与 Wi-Fi 提交位于 `ble_runtime` 和 `device_link_service`。

不可变契约固定项为 `device-link-contract.lock`。

## 宿主测试

```sh
cmake -S tests/host -B /tmp/mt-device-link -G Ninja \
  -DDEVICE_LINK_SANITIZER=none
cmake --build /tmp/mt-device-link
ctest --test-dir /tmp/mt-device-link --output-on-failure
```

在独立的构建目录中使用 `-DDEVICE_LINK_SANITIZER=address` 或 `thread`。

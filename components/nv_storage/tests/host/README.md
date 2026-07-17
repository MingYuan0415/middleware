# nv_storage 宿主测试

本套件直接编译当前模块的 `src/nv_storage.c` 与
`src/nv_storage_blob.c`，仅替换 FreeRTOS 和 ESP-IDF NVS 端口。覆盖生命周期、
失败重试、键边界、标量事务、Blob 默认值与校验、失败原子性，以及并发装载、
注册、反初始化和回调重入。

Blob 装载必须保持以下不变量：`load_begin` 先设置 `LOAD_ACTIVE`，首次获取注册表
互斥锁用于等待已经进入的注册操作；释放锁后注册表仍被冻结，因此装载可直接遍历
`s_blobs`。回调运行时不得持有注册表锁；注册、重复装载和反初始化重入必须立即
返回 `ESP_ERR_INVALID_STATE`，标量 NVS API 仍可使用。

从仓库根目录运行：

```sh
cmake -S layers/middleware/components/nv_storage/tests/host \
    -B /tmp/mt-nv-normal -G Ninja -DNV_STORAGE_SANITIZER=none
cmake --build /tmp/mt-nv-normal
ctest --test-dir /tmp/mt-nv-normal --output-on-failure

cmake -S layers/middleware/components/nv_storage/tests/host \
    -B /tmp/mt-nv-address -G Ninja -DNV_STORAGE_SANITIZER=address
cmake --build /tmp/mt-nv-address
ctest --test-dir /tmp/mt-nv-address --output-on-failure

cmake -S layers/middleware/components/nv_storage/tests/host \
    -B /tmp/mt-nv-thread -G Ninja -DNV_STORAGE_SANITIZER=thread
cmake --build /tmp/mt-nv-thread
ctest --test-dir /tmp/mt-nv-thread --output-on-failure
```

x86_64 下的 TSan 测试由 CTest 使用 `setarch -R` 关闭测试进程的 ASLR，避免
ThreadSanitizer 启动阶段的地址映射冲突。

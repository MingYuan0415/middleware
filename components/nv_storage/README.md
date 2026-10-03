# nv_storage

默认 NVS 分区的生命周期 owner：提供 K-V 标量读写与固定容量 blob 注册/默认值/校验/自动加载两层能力。

## 对外 API

公共头 `include/nv_storage.h`：

- `nv_storage_init()` / `deinit()`：初始化或释放默认 NVS 分区与 blob 注册表；本组件独占该分区生命周期。
- `nv_storage_set_u8/get_u8()`、`set_u16/get_u16()`、`set_u32/get_u32()`、`set_str/get_str()`、`set_blob/get_blob()`、`erase_key()`：标量层每次操作临时打开句柄，读写后关闭。
- `nv_storage_blob_register()`：向固定池注册 blob（key、缓冲、大小、默认值回调、可选校验回调）。
- `nv_storage_blob_load_all()`：按“读取、校验、默认值回退”逻辑加载全部已注册 blob。
- 回调类型 `nv_storage_blob_default_cb_t` / `nv_storage_blob_validate_cb_t`。

## 配置与依赖

- Kconfig：`NV_STORAGE_BLOB_POOL_SIZE`（默认 16，1..64）。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`PRIV_REQUIRES freertos nvs_flash mt_log`。
- 运行时配置来自注册时的 key、缓冲与回调；标量 key 最长 15 字节。

## 状态与并发

- 单例；静态互斥锁序列化 blob 注册表；标量 API 不依赖该锁，可在 blob 加载期间使用。
- 注册表在 blob 加载期间冻结；回调运行时服务不持有注册表锁。
- 未成功 deinit 前，其它组件不得独立 init/deinit 默认分区或跨 deinit 保留 NVS 句柄。

## 测试边界

- `tests/host` 存在，覆盖 blob 注册、加载、默认值回退与 NVS 故障注入。
- 宿主不覆盖 ESP32-S3 NVS 闪存磨损、掉电一致性与加密。
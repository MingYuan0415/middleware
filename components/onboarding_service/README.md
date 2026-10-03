# onboarding_service

持久化首次开机引导状态（PENDING / DEFERRED / COMPLETED）的轻量单例服务。

## 对外 API

公共头 `include/onboarding_service.h`：

- `onboarding_service_init()`：从 `nv_storage` 读取状态；缺失或非法字节回退为 PENDING 并写回。
- `onboarding_service_deinit()`：反初始化。
- `onboarding_service_get_state()`：复制当前状态。
- `onboarding_service_defer()` / `complete()` / `reset()`：更新并持久化状态。

## 配置与依赖

- CMake：`REQUIRES nv_storage nvs_flash`。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- 无 Kconfig；状态存储在 `nv_storage` 的固定 key `onboard_state`。

## 状态与并发

- 单例，静态状态变量；无锁、无 worker，API 同步执行。
- init 前或已 init 时按生命周期返回错误；损坏的持久化字节会被纠正为 PENDING。

## 测试边界

- 无 `tests/host`。
- 宿主不覆盖 NVS 掉电与真实首次启动路径。
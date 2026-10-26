[English](README.md) | **简体中文**

# Factory Reset Service

`factory_reset_service` 拥有持久、带版本的恢复出厂日志。它本身不擦除产品
数据。`request()` 先提交日志，然后才调用注入的重启回调；存储失败绝不重启
设备。日志操作通过重启回调串行化；该回调不得重入本服务。

## 恢复契约

启动时把有效的 `factory.reset` 标记视为一条 fail-closed 义务。根运行时在允许
网络或广播启动之前，幂等地清除每个重置域：

1. 在 Connectivity Manager 初始化之前清除持久化 Wi-Fi 档位；
2. 以 `FACTORY_RESET_GATED` 模式启动 Device Link，使授权、验证器、
   bond/CCCD 与易失传输状态在广播暂停期间收敛；
3. 在标记仍然持久且广播保持暂停时，获取持久化的慢速、非可绑定广播租约；
4. 只有这些重置域与广播前置条件均报告成功后，才清除全局标记；
5. 释放 Device Link 启动闸门，然后继续平台与网络启动。

闸门释放只是解除广播暂停的可见性提交。物理广播启动失败由 ADV owner 的有界
重试状态保留，因此它不会重新打开已完成的存储事务，也不会允许网络启动与尚未
就绪的 Device Link 服务竞争。

`recovery_pending()` 区分标记不存在与存储及完整性错误。被截断、损坏或不可读
的标记绝不被当作“没有待处理的重置”。`complete_recovery()` 是幂等的；擦除或
擦除提交失败会使先前已提交的标记在断电重启后仍可恢复。

## 宿主测试

宿主 fake 区分已暂存与已提交的 NVS 数据，并提供确定性的断电重启操作。测试覆盖
set/get/erase 失败、写入与擦除的提交失败、损坏与截断标记、并发请求接纳、
并发擦除/读取互斥、通过重启回调的 deinit 互斥，以及掉电后的恢复。Device Link
的宿主测试另行注入标记之后物理广播启动失败的情况，并验证从预先获取的租约
自动重试。

```sh
cmake -S components/factory_reset_service/tests/host \
    -B /tmp/mt-factory-reset -G Ninja \
    -DFACTORY_RESET_SERVICE_SANITIZER=none
cmake --build /tmp/mt-factory-reset
ctest --test-dir /tmp/mt-factory-reset --output-on-failure
```

sanitizer 选项也接受 `address` 和 `thread`。宿主测试不替代 ESP32-S3 的 NVS、
重启、欠压或冷启动验证。

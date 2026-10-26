[English](README.md) | **简体中文**

# Wi-Fi Service

`wifi_service` 是由 worker 拥有的单射频执行器。命令深拷贝凭据，并使用非零的
session 和 operation 身份。射频策略与持久档位选择属于其调用方，而非驱动边界。

## 操作结果与链路事实

状态快照的 `operation_complete`、`operation_result`、`operation_failure` 和
`operation_canceled` 描述由 `session_id` 与 `operation_id` 标识的那次
CONNECT/DISCONNECT。已完成的结果在下一次 CONNECT/DISCONNECT 采纳或完整初始化
之前保持粘滞。扫描快照拥有自己的身份与结果；扫描和 IPv4 更新不能覆盖状态的
终态。释放执行器所有权与完成它是两回事：成功挂起会释放未完成的 CONNECT，
而不伪造终态成功或取消。

`state`、`failure`、`last_error`、`ipv4_address` 和 `desired_connected` 描述
当前链路事实，而不是某个操作的结局。替代性的 CONNECT 不能把旧关联的 IP 报告
为自己的成功。IP_READY 之后同 epoch 的 GOT_IP 更新（包括连接态扫描）只更新
地址；重复地址不会再次发布快照，也不会完成另一次 CONNECT。

## 射频边界与密钥

射频切换会停止并排空执行器、确认 STOPPED、清除驱动凭据，并按需再次启动它。
关联处于活动状态时绝不清除配置。每个阶段都保留首个错误，并观察端口实际最终
状态。完全停止且已清除的挂起即使 stop 返回错误也提交 SUSPENDED，而控制调用
仍然返回该错误。不可用的部分状态保留清理义务，并发布 OFFLINE 与 IP0。
尝试被放弃或挂起时会擦除 worker 凭据。

若反初始化期间清除配置失败，ESP-IDF 端口保留已初始化资源，因此后续反初始化
可以重试，而不会掩盖一次失败的擦除。

## 验证边界

`tests/connectivity` 在受控射频边界下链接真实执行器与管理器，并另外针对 SDK
fake 编译真实的 ESP-IDF 端口。
这些测试不建立射频时序、DHCP 互操作性、SDK 回调覆盖率或 NVS 掉电行为。
Device Link 的线上帧格式仍定义在 `contracts/device_link/protocol.yaml`。

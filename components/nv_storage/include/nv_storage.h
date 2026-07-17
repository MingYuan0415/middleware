/**
 * @brief  NVS 非易失性存储组件 API
 *
 * K-V 标量层: 临时打开 NVS 句柄, 读写后立即关闭
 * Blob 注册层: 固定 16 槽池, 声明/默认值/校验/自动加载
 */

#ifndef __NV_STORAGE_H__
#define __NV_STORAGE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 初始化 ── */

/**
 * @brief 初始化 NVS 存储组件 (初始化默认 NVS 分区)
 *
 * 成功后本组件独占默认 NVS 分区的 lifecycle, 直到成功 deinit. 其他组件
 * 不得独立 init/deinit 默认分区或跨 nv_storage_deinit() 保留 NVS handle.
 * blob 加载期间重入 init 返回 ESP_ERR_INVALID_STATE.
 * @return ESP_OK 成功
 */
esp_err_t nv_storage_init(void);

/**
 * @brief Deinitialize NVS and clear the blob registry.
 *
 * A failed cleanup retains component ownership and may be retried. While
 * cleanup is pending, all APIs except this function reject access. An active
 * blob load rejects deinit with ESP_ERR_INVALID_STATE instead of blocking.
 *
 * @return ESP_OK when released; ESP_ERR_INVALID_STATE during an active load;
 *         otherwise the retained NVS cleanup error.
 */
esp_err_t nv_storage_deinit(void);

/* ── K-V 标量层 (每次操作 open -> read/write -> close) ── */

/**
 * @brief Store one unsigned 8-bit value.
 * @param key is a valid NVS key of at most 15 bytes.
 * @param value is the value to store.
 * @return ESP_OK on success, otherwise an ESP-IDF NVS error.
 */
esp_err_t nv_storage_set_u8(const char *key, uint8_t value);
/**
 * @brief Read one unsigned 8-bit value.
 * @param key is a valid NVS key of at most 15 bytes.
 * @param output receives the stored value.
 * @return ESP_OK on success, otherwise an ESP-IDF NVS error.
 */
esp_err_t nv_storage_get_u8(const char *key, uint8_t *output);

/**
 * @brief Store one unsigned 16-bit value.
 * @param key is a valid NVS key of at most 15 bytes.
 * @param value is the value to store.
 * @return ESP_OK on success, otherwise an ESP-IDF NVS error.
 */
esp_err_t nv_storage_set_u16(const char *key, uint16_t value);
/**
 * @brief Read one unsigned 16-bit value.
 * @param key is a valid NVS key of at most 15 bytes.
 * @param output receives the stored value.
 * @return ESP_OK on success, otherwise an ESP-IDF NVS error.
 */
esp_err_t nv_storage_get_u16(const char *key, uint16_t *output);

/**
 * @brief Store one unsigned 32-bit value.
 * @param key is a valid NVS key of at most 15 bytes.
 * @param value is the value to store.
 * @return ESP_OK on success, otherwise an ESP-IDF NVS error.
 */
esp_err_t nv_storage_set_u32(const char *key, uint32_t value);
/**
 * @brief Read one unsigned 32-bit value.
 * @param key is a valid NVS key of at most 15 bytes.
 * @param output receives the stored value.
 * @return ESP_OK on success, otherwise an ESP-IDF NVS error.
 */
esp_err_t nv_storage_get_u32(const char *key, uint32_t *output);

/**
 * @brief 写字符串到 NVS
 * @param key  键名 (NVS 键最长 15 字符)
 * @param val  字符串值 (null 结尾)
 * @return ESP_OK 成功
 */
esp_err_t nv_storage_set_str(const char *key, const char *val);

/**
 * @brief 从 NVS 读字符串
 * @param key   键名
 * @param out   输出缓冲 (调用方提供)
 * @param size  入: 缓冲容量; 出: 实际字符串长度 (含 '\0')
 *              若缓冲不足则返回 ESP_ERR_NVS_INVALID_LENGTH,
 *              并通过 *size 返回所需大小
 * @return ESP_OK 成功
 */
esp_err_t nv_storage_get_str(const char *key, char *out, size_t *size);

/**
 * @brief 写二进制 blob 到 NVS
 * @param key   键名
 * @param data  数据指针
 * @param len   数据长度
 * @return ESP_OK 成功
 */
esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t len);

/**
 * @brief 从 NVS 读二进制 blob
 * @param key   键名
 * @param out   输出缓冲 (调用方提供)
 * @param size  入: 缓冲容量; 出: 实际数据长度
 *              若缓冲不足则返回 ESP_ERR_NVS_INVALID_LENGTH,
 *              并通过 *size 返回所需大小
 * @return ESP_OK 成功
 */
esp_err_t nv_storage_get_blob(const char *key, void *out, size_t *size);

/**
 * @brief 从 NVS 擦除指定键
 * @param key  键名
 * @return ESP_OK 成功; ESP_ERR_NVS_NOT_FOUND 键不存在
 */
esp_err_t nv_storage_erase_key(const char *key);

/* ── Blob 注册层 (固定池 16 槽) ── */

/**
 * @brief blob 默认值生成回调
 *        组件首次启动或校验失败时调用, 将默认值写入候选缓冲区
 * @param data  组件拥有的临时候选缓冲区, 仅在回调期间有效, 不得留存
 * @param size  内存大小 (注册时传入的值)
 * @return ESP_OK 成功
 */
typedef esp_err_t (*nv_storage_blob_default_cb_t)(void *data, size_t size);

/**
 * @brief blob 校验回调
 * @param data  NVS 中读出的临时候选数据, 仅在回调期间有效, 不得留存
 * @param size  数据大小
 * @return true  数据有效
 * @return false 数据无效, 将触发默认值重新生成
 */
typedef bool (*nv_storage_blob_validate_cb_t)(const void *data, size_t size);

/**
 * @brief 注册一个 blob 到固定池
 *
 * 注册后调用 nv_storage_blob_load_all() 执行加载逻辑:
 *   1. NVS 中存在且校验通过 → 直接加载到 data 指针
 *   2. NVS 中不存在 → 调用 default_cb 生成初值 → 写入 NVS
 *   3. NVS 中存在但 size 不匹配 → 调用 default_cb 重新生成 → 写回
 *   4. NVS 中存在但 validate_cb 返回 false → 调用 default_cb → 写回
 *
 * @param key          键名 (注册时复制, 调用返回后无需继续存活)
 * @param data         数据缓冲区指针 (blob 加载/默认值写入目标), 必须保持
 *                     有效直到成功 deinit 清空注册表
 * @param size         数据缓冲区大小
 * @param default_cb   默认值生成回调 (不可为 NULL), 函数必须保持有效直到
 *                     成功 deinit
 * @param validate_cb  校验回调 (可为 NULL, 表示只校验 size), 非 NULL 时函数
 *                     必须保持有效直到成功 deinit
 * 加载期间注册表被冻结, 重入注册返回 ESP_ERR_INVALID_STATE.
 *
 * @return ESP_OK 成功; ESP_ERR_NO_MEM 池满 (16 槽全部占用)
 */
esp_err_t nv_storage_blob_register(const char *key, void *data, size_t size,
                                   nv_storage_blob_default_cb_t default_cb,
                                   nv_storage_blob_validate_cb_t validate_cb);

/**
 * @brief 加载所有已注册 blob
 *
 * 对每个已注册的 blob 条目, 执行 NVS 读取 → 校验 → 默认值回退逻辑.
 * 回调运行时不持有注册表锁; 加载、注册或 deinit 重入会返回
 * ESP_ERR_INVALID_STATE, 标量 NVS API 仍可使用.
 * 通常仅在系统启动时调用一次
 *
 * @return ESP_OK 全部加载成功; 否则返回第一个失败项的 err
 */
esp_err_t nv_storage_blob_load_all(void);

#ifdef __cplusplus
}
#endif

#endif /* __NV_STORAGE_H__ */

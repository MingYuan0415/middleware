#ifndef __MT_LOG_H__
#define __MT_LOG_H__

#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DBG_LVL values */
#define DBG_ERROR   1
#define DBG_WARN    2
#define DBG_INFO    3
#define DBG_DEBUG   4
#define DBG_VERBOSE 5

/* LOG_E - Error (always enabled when DBG_LVL >= DBG_ERROR) */
#if DBG_LVL >= DBG_ERROR
#define LOG_E(fmt, ...) ESP_LOGE(DBG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_E(fmt, ...) ((void)0)
#endif

/* LOG_W - Warning */
#if DBG_LVL >= DBG_WARN
#define LOG_W(fmt, ...) ESP_LOGW(DBG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_W(fmt, ...) ((void)0)
#endif

/* LOG_I - Info */
#if DBG_LVL >= DBG_INFO
#define LOG_I(fmt, ...) ESP_LOGI(DBG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_I(fmt, ...) ((void)0)
#endif

/* LOG_D - Debug */
#if DBG_LVL >= DBG_DEBUG
#define LOG_D(fmt, ...) ESP_LOGD(DBG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_D(fmt, ...) ((void)0)
#endif

/* LOG_V - Verbose */
#if DBG_LVL >= DBG_VERBOSE
#define LOG_V(fmt, ...) ESP_LOGV(DBG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_V(fmt, ...) ((void)0)
#endif

/**
 * @brief Save an ESP-IDF error location and jump to the function exit label.
 *
 * @note The caller must declare an error variable and a line variable, and
 *       provide an `exit` label in the same function. The error expression is
 *       evaluated once before the jump decision.
 *
 * @param err is the ESP-IDF error value to check.
 * @param line receives the source line where the error was detected.
 */
#define MT_ERROR_HANDLE(err, line) \
    do \
    { \
        if ((err) != ESP_OK) \
        { \
            (line) = __LINE__; \
            goto exit; \
        } \
    } while (0)

/**
 * @brief Initialize the process logging facade.
 *
 * @return ESP_OK when logging is ready.
 */
esp_err_t mt_log_init(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __MT_LOG_H__ */

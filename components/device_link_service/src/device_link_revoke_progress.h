#ifndef __DEVICE_LINK_REVOKE_PROGRESS_H__
#define __DEVICE_LINK_REVOKE_PROGRESS_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Advance a journaled binding revoke and report durable completion.
 * @return ESP_OK only after the revoke journal is absent;
 *         ESP_ERR_NOT_FINISHED while deletion remains pending;
 *         otherwise the queue or storage error.
 */
esp_err_t device_link_revoke_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_REVOKE_PROGRESS_H__ */

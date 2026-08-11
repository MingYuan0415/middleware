#ifndef __HOST_DL_SECURITY_NV_STORAGE_H__
#define __HOST_DL_SECURITY_NV_STORAGE_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In-memory NVS fake: one blob key with commit/erase semantics, plus a
 * test hook to reset state between cases. */
esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t len);
esp_err_t nv_storage_get_blob(const char *key, void *out, size_t *size);
esp_err_t nv_storage_erase_key(const char *key);
void nv_storage_fake_reset(void);
size_t nv_storage_fake_blob_len(void);
void nv_storage_fake_fail_next_get(esp_err_t result);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_DL_SECURITY_NV_STORAGE_H__ */

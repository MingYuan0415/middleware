#ifndef __HOST_DL_SECURITY_NV_STORAGE_H__
#define __HOST_DL_SECURITY_NV_STORAGE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In-memory NVS fake with staged/committed semantics: set_blob and
 * erase_key stage the write and publish it only on the internal commit
 * (matching production nv_storage, where nvs_set_blob + nvs_commit run in
 * one call). A failed commit leaves the staged value readable but not
 * durable; a power cycle discards it. */
esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t len);
esp_err_t nv_storage_get_blob(const char *key, void *out, size_t *size);
esp_err_t nv_storage_erase_key(const char *key);
void nv_storage_fake_reset(void);
size_t nv_storage_fake_blob_len(void);
void nv_storage_fake_fail_next_set(esp_err_t result);
void nv_storage_fake_fail_next_get(esp_err_t result);
void nv_storage_fake_fail_next_erase(esp_err_t result);
void nv_storage_fake_fail_next_commit(esp_err_t result);
void nv_storage_fake_power_cycle(void);
bool nv_storage_fake_commit_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_DL_SECURITY_NV_STORAGE_H__ */

#ifndef __HOST_FACTORY_RESET_NV_STORAGE_H__
#define __HOST_FACTORY_RESET_NV_STORAGE_H__

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t len);
esp_err_t nv_storage_get_blob(const char *key, void *out, size_t *size);
esp_err_t nv_storage_erase_key(const char *key);

void nv_storage_fake_reset(void);
void nv_storage_fake_power_cycle(void);
void nv_storage_fake_fail_next_set(esp_err_t result);
void nv_storage_fake_fail_next_get(esp_err_t result);
void nv_storage_fake_fail_next_erase(esp_err_t result);
void nv_storage_fake_fail_next_commit(esp_err_t result);
void nv_storage_fake_block_next_set(void);
void nv_storage_fake_wait_set_blocked(void);
void nv_storage_fake_release_blocked_set(void);
size_t nv_storage_fake_committed_blob_len(void);
bool nv_storage_fake_commit_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_FACTORY_RESET_NV_STORAGE_H__ */

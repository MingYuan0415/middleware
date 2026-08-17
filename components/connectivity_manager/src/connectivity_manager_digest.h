#ifndef __CONNECTIVITY_MANAGER_DIGEST_H__
#define __CONNECTIVITY_MANAGER_DIGEST_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t connectivity_manager_digest_sha256(
    const uint8_t *data, size_t length, uint8_t digest[32U]);

#endif /* __CONNECTIVITY_MANAGER_DIGEST_H__ */

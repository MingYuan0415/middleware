#ifndef __DEVICE_LINK_DIGEST_H__
#define __DEVICE_LINK_DIGEST_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Digest one bounded message for replay comparison. */
esp_err_t device_link_digest_sha256(
    const uint8_t *data, size_t length,
    uint8_t digest[32U], void *arg);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_DIGEST_H__ */

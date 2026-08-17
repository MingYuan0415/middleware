#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "psa/crypto.h"

#include "connectivity_manager_digest.h"

esp_err_t connectivity_manager_digest_sha256(
    const uint8_t *data, size_t length, uint8_t digest[32U])
{
    if ((data == NULL && length != 0U) || digest == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    size_t digest_length = 0U;
    psa_status_t status = psa_crypto_init();

    if (status == PSA_SUCCESS)
    {
        status = psa_hash_compute(
                     PSA_ALG_SHA_256, data, length, digest, 32U,
                     &digest_length);
    }
    if (status != PSA_SUCCESS || digest_length != 32U)
    {
        memset(digest, 0, 32U);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

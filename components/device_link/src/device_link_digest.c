#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tinycrypt/sha256.h"

#include "device_link_digest.h"

esp_err_t device_link_digest_sha256(
    const uint8_t *data, size_t length, uint8_t digest[32U], void *arg)
{
    (void)arg;
    if ((data == NULL && length != 0U) || digest == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    struct tc_sha256_state_struct state;

    memset(&state, 0, sizeof(state));
    if (tc_sha256_init(&state) != 1 ||
            tc_sha256_update(&state, data, length) != 1 ||
            tc_sha256_final(digest, &state) != 1)
    {
        memset(&state, 0, sizeof(state));
        memset(digest, 0, 32U);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&state, 0, sizeof(state));
    return ESP_OK;
}

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_srp.h"

#define TEST_USERNAME "microtech"
#define SRP_VERIFIER_BYTES 384U

esp_err_t esp_srp_gen_salt_verifier(
    const char *username, int username_len,
    const char *pass, int pass_len,
    char **bytes_salt, int salt_len,
    char **verifier, int *verifier_len)
{
    assert(username != NULL && pass != NULL);
    (void)username_len;
    if (salt_len <= 0 || pass_len <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char *salt = malloc((size_t)salt_len);
    char *out = malloc(SRP_VERIFIER_BYTES);

    if (salt == NULL || out == NULL)
    {
        free(salt);
        free(out);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < salt_len; ++i)
    {
        salt[i] = (char)(0x40 + i);
    }
    for (int i = 0; i < (int)SRP_VERIFIER_BYTES; ++i)
    {
        out[i] = (char)(pass[i % pass_len] + (i % 7));
    }
    *bytes_salt = salt;
    *verifier = out;
    *verifier_len = (int)SRP_VERIFIER_BYTES;
    return ESP_OK;
}

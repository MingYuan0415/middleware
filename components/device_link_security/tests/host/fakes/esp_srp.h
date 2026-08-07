#ifndef __HOST_DL_SECURITY_ESP_SRP_H__
#define __HOST_DL_SECURITY_ESP_SRP_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Deterministic host fake for esp_srp_gen_salt_verifier().
 *
 * The salt is a fixed 16-byte pattern and the verifier is derived from
 * the password, so tests can predict the captured parameters.
 */
esp_err_t esp_srp_gen_salt_verifier(
    const char *username, int username_len,
    const char *pass, int pass_len,
    char **bytes_salt, int salt_len,
    char **verifier, int *verifier_len);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_DL_SECURITY_ESP_SRP_H__ */

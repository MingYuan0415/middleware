#ifndef __PROVISIONING_HOST_ESP_SRP_H__
#define __PROVISIONING_HOST_ESP_SRP_H__

#include "esp_err.h"

esp_err_t esp_srp_gen_salt_verifier(
    const char *username, int username_length,
    const char *password, int password_length,
    char **salt, int salt_length,
    char **verifier, int *verifier_length);

#endif /* __PROVISIONING_HOST_ESP_SRP_H__ */

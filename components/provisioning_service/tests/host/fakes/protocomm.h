#ifndef __PROVISIONING_HOST_PROTOCOMM_H__
#define __PROVISIONING_HOST_PROTOCOMM_H__

#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

typedef struct protocomm protocomm_t;
typedef struct protocomm_security
{
    unsigned marker;
} protocomm_security_t;

typedef esp_err_t (*protocomm_req_handler_t)(
    uint32_t session_id, const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length, void *private_data);

protocomm_t *protocomm_new(void);
void protocomm_delete(protocomm_t *protocomm);
esp_err_t protocomm_set_security(
    protocomm_t *protocomm, const char *endpoint,
    const protocomm_security_t *security, const void *parameters);
esp_err_t protocomm_set_version(
    protocomm_t *protocomm, const char *endpoint, const char *version);
esp_err_t protocomm_add_endpoint(
    protocomm_t *protocomm, const char *endpoint,
    protocomm_req_handler_t handler, void *private_data);

#endif /* __PROVISIONING_HOST_PROTOCOMM_H__ */

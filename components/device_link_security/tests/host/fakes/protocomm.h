#ifndef __HOST_DL_SECURITY_PROTOCOMM_H__
#define __HOST_DL_SECURITY_PROTOCOMM_H__

#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

typedef struct protocomm protocomm_t;
typedef struct protocomm_security
{
    int ver;
} protocomm_security_t;

typedef struct protocomm_security2_params
{
    const char *salt;
    int salt_len;
    const char *verifier;
    uint16_t verifier_len;
} protocomm_security2_params_t;

typedef esp_err_t (*protocomm_req_handler_t)(
    uint32_t session_id, const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length, void *private_data);

protocomm_t *protocomm_new(void);
void protocomm_delete(protocomm_t *protocomm);
esp_err_t protocomm_set_security(
    protocomm_t *protocomm, const char *endpoint,
    const protocomm_security_t *security, const void *parameters);
esp_err_t protocomm_add_endpoint(
    protocomm_t *protocomm, const char *endpoint,
    protocomm_req_handler_t handler, void *private_data);
esp_err_t protocomm_open_session(protocomm_t *protocomm, uint32_t session_id);
esp_err_t protocomm_close_session(protocomm_t *protocomm, uint32_t session_id);
esp_err_t protocomm_req_handle(
    protocomm_t *protocomm, const char *endpoint, uint32_t session_id,
    const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length);

#endif /* __HOST_DL_SECURITY_PROTOCOMM_H__ */

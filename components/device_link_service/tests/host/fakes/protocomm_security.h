#ifndef __HOST_DL_SECURITY_PROTOCOMM_SECURITY_H__
#define __HOST_DL_SECURITY_PROTOCOMM_SECURITY_H__

#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *protocomm_security_handle_t;

typedef struct protocomm_security2_params
{
    const char *salt;
    uint16_t salt_len;
    const char *verifier;
    uint16_t verifier_len;
} protocomm_security2_params_t;

typedef struct protocomm_security
{
    int ver;
    uint8_t patch_ver;
    esp_err_t (*init)(protocomm_security_handle_t *handle);
    esp_err_t (*cleanup)(protocomm_security_handle_t handle);
    esp_err_t (*new_transport_session)(protocomm_security_handle_t handle,
                                       uint32_t session_id);
    esp_err_t (*close_transport_session)(protocomm_security_handle_t handle,
                                         uint32_t session_id);
    esp_err_t (*security_req_handler)(protocomm_security_handle_t handle,
                                      const void *sec_params,
                                      uint32_t session_id,
                                      const uint8_t *inbuf, ssize_t inlen,
                                      uint8_t **outbuf, ssize_t *outlen,
                                      void *priv_data);
    esp_err_t (*encrypt)(protocomm_security_handle_t handle,
                         uint32_t session_id,
                         const uint8_t *inbuf, ssize_t inlen,
                         uint8_t **outbuf, ssize_t *outlen);
    esp_err_t (*decrypt)(protocomm_security_handle_t handle,
                         uint32_t session_id,
                         const uint8_t *inbuf, ssize_t inlen,
                         uint8_t **outbuf, ssize_t *outlen);
} protocomm_security_t;

#ifdef __cplusplus
}
#endif

#endif /* __HOST_DL_SECURITY_PROTOCOMM_SECURITY_H__ */

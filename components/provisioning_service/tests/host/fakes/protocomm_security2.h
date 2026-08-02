#ifndef __PROVISIONING_HOST_PROTOCOMM_SECURITY2_H__
#define __PROVISIONING_HOST_PROTOCOMM_SECURITY2_H__

#include <stdint.h>

#include "protocomm.h"

typedef struct protocomm_security2_params
{
    const char *salt;
    uint16_t salt_len;
    const char *verifier;
    uint16_t verifier_len;
} protocomm_security2_params_t;

extern const protocomm_security_t protocomm_security2;

#endif /* __PROVISIONING_HOST_PROTOCOMM_SECURITY2_H__ */

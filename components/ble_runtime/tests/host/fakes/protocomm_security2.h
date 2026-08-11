#ifndef __HOST_DL_SECURITY_PROTOCOMM_SECURITY2_H__
#define __HOST_DL_SECURITY_PROTOCOMM_SECURITY2_H__

#include "protocomm_security.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Security 2 vtable fake: mirrors the production driver (init, transport
 * sessions, structured Cmd0/Cmd1 responses, AES-GCM encrypt/decrypt).
 * Decrypt strips a 16-byte tag and encrypt appends one, so the adapter and
 * request callback paths are exercised end to end. */
extern const protocomm_security_t protocomm_security2;

#ifdef __cplusplus
}
#endif

#endif /* __HOST_DL_SECURITY_PROTOCOMM_SECURITY2_H__ */

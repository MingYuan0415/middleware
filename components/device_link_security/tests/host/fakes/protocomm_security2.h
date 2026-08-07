#ifndef __HOST_DL_SECURITY_PROTOCOMM_SECURITY2_H__
#define __HOST_DL_SECURITY_PROTOCOMM_SECURITY2_H__

#include "protocomm_security.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Security 2 vtable fake: mirrors the production driver (init, transport
 * sessions, SRP handshake handler, AES-GCM encrypt/decrypt). The fake
 * handshake echoes the request prefixed with "resp:", decrypt strips a
 * 16-byte tag and encrypts a 16-byte tag, so the adapter logic and the
 * request callback path are exercised end to end. */
extern const protocomm_security_t protocomm_security2;

#ifdef __cplusplus
}
#endif

#endif /* __HOST_DL_SECURITY_PROTOCOMM_SECURITY2_H__ */

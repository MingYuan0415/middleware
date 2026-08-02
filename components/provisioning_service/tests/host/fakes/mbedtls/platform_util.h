#ifndef __PROVISIONING_HOST_MBEDTLS_PLATFORM_UTIL_H__
#define __PROVISIONING_HOST_MBEDTLS_PLATFORM_UTIL_H__

#include <stddef.h>
#include <stdint.h>

void host_mbedtls_platform_zeroize(void *buffer, size_t length);

static inline void mbedtls_platform_zeroize(void *buffer, size_t length)
{
    host_mbedtls_platform_zeroize(buffer, length);
}

#endif /* __PROVISIONING_HOST_MBEDTLS_PLATFORM_UTIL_H__ */

#ifndef __HOST_BLE_RUNTIME_SEC_FAKE_H__
#define __HOST_BLE_RUNTIME_SEC_FAKE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ble_link_sec_fake_reset(void);
void ble_link_sec_fake_fail_next_handshake(void);
uint32_t ble_link_sec_fake_handshake_count(void);
uint32_t ble_link_sec_fake_encrypt_count(void);
uint32_t ble_link_sec_fake_decrypt_count(void);
const char *ble_link_sec_fake_last_salt(size_t *len);
const char *ble_link_sec_fake_last_verifier(size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_BLE_RUNTIME_SEC_FAKE_H__ */

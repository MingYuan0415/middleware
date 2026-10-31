#ifndef __MUSIC_LOGIN_PROTOCOL_H__
#define __MUSIC_LOGIN_PROTOCOL_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Parsed QQ QR login poll state. */
typedef enum
{
    MUSIC_LOGIN_QQ_WAITING = 0,
    MUSIC_LOGIN_QQ_SCANNED,
    MUSIC_LOGIN_QQ_SUCCESS,
    MUSIC_LOGIN_QQ_FAILED,
} music_login_qq_state_t;

/**
 * @brief QQ ptlogin hash33 used by ptqrtoken and g_tk.
 *
 * @param value Input string, may be NULL.
 * @param initial Initial hash value.
 * @return Positive 31-bit hash.
 */
uint32_t music_login_hash33(const char *value, uint32_t initial);

/**
 * @brief Parse one ptuiCB response from ptqrlogin.
 *
 * @param body Response body.
 * @param musicid Output QQ number on success.
 * @param musicid_size Output buffer size.
 * @param auth_url Output OAuth authorization URL on success.
 * @param auth_url_size Output buffer size.
 * @return Parsed login state.
 */
music_login_qq_state_t music_login_parse_ptui_cb(
    const char *body, char *musicid, size_t musicid_size,
    char *auth_url, size_t auth_url_size);

/**
 * @brief Parse the leading name=value pair of a Set-Cookie header.
 *
 * @return true when a non-empty name and value were copied.
 */
bool music_login_cookie_parse(const char *header, char *name, size_t name_size,
                              char *value, size_t value_size);

/** @brief Check the eight-byte PNG signature. */
bool music_login_png_signature(const uint8_t *data, size_t size);

/**
 * @brief Read one query parameter from a URL.
 *
 * The value is percent-decoded in place before it is copied out.
 */
bool music_login_query_value(const char *url, const char *key,
                             char *output, size_t output_size);

/**
 * @brief Append one application/x-www-form-urlencoded key/value pair.
 *
 * @param buffer Output buffer.
 * @param buffer_size Output buffer size.
 * @param used In/out used byte count, excluding the terminator.
 * @param key Field name, already URL-safe.
 * @param value Field value, encoded by this function.
 * @return true on success.
 */
bool music_login_form_add(char *buffer, size_t buffer_size, size_t *used,
                          const char *key, const char *value);

#endif /* __MUSIC_LOGIN_PROTOCOL_H__ */

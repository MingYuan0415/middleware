#include "music_login_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void _copy_range(const char *start, const char *end, char *output,
                        size_t output_size)
{
    if (output == NULL || output_size == 0U || start == NULL || end == NULL ||
            end <= start)
    {
        return;
    }
    size_t length = (size_t)(end - start);
    if (length >= output_size)
    {
        length = output_size - 1U;
    }
    memcpy(output, start, length);
    output[length] = '\0';
}

uint32_t music_login_hash33(const char *value, uint32_t initial)
{
    uint32_t hash = initial;
    if (value == NULL)
    {
        return hash & 0x7FFFFFFFU;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
            *cursor != '\0'; ++cursor)
    {
        hash = (hash << 5) + hash + *cursor;
    }
    return hash & 0x7FFFFFFFU;
}

static int _ptui_code(const char *body)
{
    static const char prefix[] = "ptuiCB('";
    const char *start = strstr(body, prefix);
    if (start == NULL)
    {
        return -1;
    }
    start += sizeof(prefix) - 1U;
    char digits[8];
    size_t count = 0U;
    while (start[count] >= '0' && start[count] <= '9' &&
            count < sizeof(digits) - 1U)
    {
        digits[count] = start[count];
        ++count;
    }
    if (count == 0U)
    {
        return -1;
    }
    digits[count] = '\0';
    return atoi(digits);
}

static bool _extract_success_fields(const char *body, char *musicid,
                                    size_t musicid_size, char *auth_url,
                                    size_t auth_url_size)
{
    const char *uin_key = strstr(body, "&uin=");
    if (uin_key == NULL)
    {
        return false;
    }
    uin_key += strlen("&uin=");
    const char *uin_end = strchr(uin_key, '&');
    if (uin_end == NULL || uin_end == uin_key)
    {
        return false;
    }
    const char *url_start = strstr(body, "'https:");
    if (url_start == NULL)
    {
        return false;
    }
    ++url_start;
    const char *url_end = strchr(url_start, '\'');
    if (url_end == NULL || url_end == url_start)
    {
        return false;
    }
    _copy_range(uin_key, uin_end, musicid, musicid_size);
    _copy_range(url_start, url_end, auth_url, auth_url_size);
    return true;
}

music_login_qq_state_t music_login_parse_ptui_cb(
    const char *body, char *musicid, size_t musicid_size,
    char *auth_url, size_t auth_url_size)
{
    if (musicid != NULL && musicid_size > 0U)
    {
        musicid[0] = '\0';
    }
    if (auth_url != NULL && auth_url_size > 0U)
    {
        auth_url[0] = '\0';
    }
    if (body == NULL)
    {
        return MUSIC_LOGIN_QQ_FAILED;
    }

    const int code = _ptui_code(body);
    if (code == 66)
    {
        return MUSIC_LOGIN_QQ_WAITING;
    }
    if (code == 67)
    {
        return MUSIC_LOGIN_QQ_SCANNED;
    }
    if (code == 65)
    {
        return MUSIC_LOGIN_QQ_FAILED;
    }
    if (code == 0)
    {
        return _extract_success_fields(body, musicid, musicid_size,
                                       auth_url, auth_url_size) ?
               MUSIC_LOGIN_QQ_SUCCESS : MUSIC_LOGIN_QQ_FAILED;
    }

    /* Older responses may omit a usable numeric code; fall back to the
     * fixed Chinese status text used by the page. */
    if (strstr(body, "二维码未失效") != NULL)
    {
        return MUSIC_LOGIN_QQ_WAITING;
    }
    if (strstr(body, "二维码认证中") != NULL)
    {
        return MUSIC_LOGIN_QQ_SCANNED;
    }
    if (strstr(body, "二维码已失效") != NULL ||
            strstr(body, "本次登录已被拒绝") != NULL)
    {
        return MUSIC_LOGIN_QQ_FAILED;
    }
    if (strstr(body, "登录成功") != NULL &&
            _extract_success_fields(body, musicid, musicid_size,
                                    auth_url, auth_url_size))
    {
        return MUSIC_LOGIN_QQ_SUCCESS;
    }
    return MUSIC_LOGIN_QQ_FAILED;
}

bool music_login_cookie_parse(const char *header, char *name, size_t name_size,
                              char *value, size_t value_size)
{
    if (name != NULL && name_size > 0U)
    {
        name[0] = '\0';
    }
    if (value != NULL && value_size > 0U)
    {
        value[0] = '\0';
    }
    if (header == NULL || name == NULL || value == NULL ||
            name_size == 0U || value_size == 0U)
    {
        return false;
    }
    while (*header == ' ' || *header == '\t')
    {
        ++header;
    }
    const char *equals = strchr(header, '=');
    const char *semicolon = strchr(header, ';');
    if (equals == NULL || (semicolon != NULL && equals > semicolon) ||
            equals == header)
    {
        return false;
    }
    const char *name_end = equals;
    while (name_end > header && (name_end[-1] == ' ' ||
                                 name_end[-1] == '\t'))
    {
        --name_end;
    }
    const char *value_start = equals + 1;
    const char *value_end = semicolon == NULL ? header + strlen(header) :
                            semicolon;
    while (value_end > value_start && (value_end[-1] == ' ' ||
                                       value_end[-1] == '\t'))
    {
        --value_end;
    }
    if (value_end == value_start)
    {
        return false;
    }
    _copy_range(header, name_end, name, name_size);
    _copy_range(value_start, value_end, value, value_size);
    return name[0] != '\0' && value[0] != '\0';
}

bool music_login_png_signature(const uint8_t *data, size_t size)
{
    static const uint8_t signature[] =
    {
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU
    };
    return data != NULL && size >= sizeof(signature) &&
           memcmp(data, signature, sizeof(signature)) == 0;
}

static int _hex_value(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

bool music_login_query_value(const char *url, const char *key,
                             char *output, size_t output_size)
{
    if (output != NULL && output_size > 0U)
    {
        output[0] = '\0';
    }
    if (url == NULL || key == NULL || key[0] == '\0' || output == NULL ||
            output_size == 0U)
    {
        return false;
    }
    const size_t key_length = strlen(key);
    const char *cursor = url;
    while ((cursor = strstr(cursor, key)) != NULL)
    {
        const bool boundary = cursor == url || cursor[-1] == '?' ||
                              cursor[-1] == '&';
        if (!boundary || cursor[key_length] != '=')
        {
            cursor += key_length;
            continue;
        }
        const char *start = cursor + key_length + 1U;
        const char *end = start;
        while (*end != '\0' && *end != '&' && *end != '#')
        {
            ++end;
        }
        size_t used = 0U;
        for (const char *value = start; value < end; ++value)
        {
            char decoded = *value;
            if (*value == '%' && value + 2U < end)
            {
                const int high = _hex_value(value[1]);
                const int low = _hex_value(value[2]);
                if (high >= 0 && low >= 0)
                {
                    decoded = (char)((high << 4) | low);
                    value += 2U;
                }
            }
            if (used + 1U >= output_size)
            {
                output[0] = '\0';
                return false;
            }
            output[used++] = decoded;
        }
        output[used] = '\0';
        return used > 0U;
    }
    return false;
}

static bool _unreserved(unsigned char value)
{
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') ||
           value == '-' || value == '_' || value == '.' || value == '~';
}

bool music_login_form_add(char *buffer, size_t buffer_size, size_t *used,
                          const char *key, const char *value)
{
    if (buffer == NULL || buffer_size == 0U || used == NULL || key == NULL)
    {
        return false;
    }
    if (*used >= buffer_size)
    {
        return false;
    }
    const char *text = value == NULL ? "" : value;
    size_t encoded_length = 0U;
    for (const unsigned char *cursor = (const unsigned char *)text;
            *cursor != '\0'; ++cursor)
    {
        encoded_length += _unreserved(*cursor) ? 1U :
                          (*cursor == ' ' ? 1U : 3U);
    }
    const size_t separator = *used == 0U ? 0U : 1U;
    const size_t needed = *used + separator + strlen(key) + 1U +
                          encoded_length + 1U;
    if (needed > buffer_size)
    {
        return false;
    }
    if (separator != 0U)
    {
        buffer[(*used)++] = '&';
    }
    const size_t key_length = strlen(key);
    memcpy(buffer + *used, key, key_length);
    *used += key_length;
    buffer[(*used)++] = '=';
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *cursor = (const unsigned char *)text;
            *cursor != '\0'; ++cursor)
    {
        if (_unreserved(*cursor))
        {
            buffer[(*used)++] = (char) * cursor;
        }
        else if (*cursor == ' ')
        {
            buffer[(*used)++] = '+';
        }
        else
        {
            buffer[(*used)++] = '%';
            buffer[(*used)++] = hex[*cursor >> 4];
            buffer[(*used)++] = hex[*cursor & 0x0FU];
        }
    }
    buffer[*used] = '\0';
    return true;
}

bool music_login_code_is_auth_expired(int code)
{
    return code == 1000 || code == 104400 || code == 104401;
}

static bool _credential_id_valid(const char *musicid)
{
    if (musicid == NULL || musicid[0] == '\0')
    {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)musicid;
            *cursor != '\0'; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9')
        {
            return false;
        }
    }
    return true;
}

static bool _credential_key_valid(const char *musickey)
{
    if (musickey == NULL || musickey[0] == '\0')
    {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)musickey;
            *cursor != '\0'; ++cursor)
    {
        if (*cursor <= 0x20U || *cursor >= 0x7FU ||
                *cursor == ';' || *cursor == '&' ||
                *cursor == '=' || *cursor == '\'' ||
                *cursor == '"')
        {
            return false;
        }
    }
    return true;
}

bool music_login_build_session_check_url(char *output, size_t output_size,
        const char *musicid,
        const char *musickey)
{
    if (output == NULL || output_size == 0U ||
            !_credential_id_valid(musicid) ||
            !_credential_key_valid(musickey))
    {
        return false;
    }
    char g_tk[24];
    (void)snprintf(g_tk, sizeof(g_tk), "%u",
                   (unsigned)music_login_hash33(musickey, 5381U));
    const int written = snprintf(
                            output, output_size,
                            "https://c6.y.qq.com/rsc/fcgi-bin/"
                            "fcg_get_profile_homepage.fcg"
                            "?g_tk=%s&format=json&inCharset=utf-8"
                            "&outCharset=utf-8&notice=0&cid=205360838"
                            "&needNewCode=0&loginUin=%s&hostUin=0"
                            "&userid=%s&reqfrom=1",
                            g_tk, musicid, musicid);
    return written > 0 && (size_t)written < output_size;
}

bool music_login_build_credential_cookie(char *output, size_t output_size,
        const char *musicid,
        const char *musickey)
{
    if (output == NULL || output_size == 0U ||
            !_credential_id_valid(musicid) ||
            !_credential_key_valid(musickey))
    {
        return false;
    }
    const int written = snprintf(
                            output, output_size,
                            "uin=%s; qqmusic_uin=%s; qm_keyst=%s; "
                            "qqmusic_key=%s",
                            musicid, musicid, musickey, musickey);
    return written > 0 && (size_t)written < output_size;
}

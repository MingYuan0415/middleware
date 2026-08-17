#include <stddef.h>
#include <string.h>

#include "wifi_service.h"

static bool _wifi_service_ascii_hex(const char *value, size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        const char item = value[index];

        if (!((item >= '0' && item <= '9') ||
                (item >= 'a' && item <= 'f') ||
                (item >= 'A' && item <= 'F')))
        {
            return false;
        }
    }
    return true;
}

bool wifi_service_credentials_valid(
    const wifi_service_credentials_t *credentials)
{
    if (credentials == NULL || credentials->ssid == NULL ||
            credentials->ssid_length == 0U ||
            credentials->ssid_length > WIFI_SERVICE_SSID_MAX_BYTES ||
            memchr(credentials->ssid, '\0', credentials->ssid_length) != NULL)
    {
        return false;
    }
    if (credentials->security == WIFI_SERVICE_SECURITY_OPEN)
    {
        return credentials->password_length == 0U;
    }
    if (credentials->security != WIFI_SERVICE_SECURITY_PERSONAL ||
            credentials->password == NULL ||
            credentials->password_length >
            WIFI_SERVICE_PASSWORD_MAX_BYTES ||
            memchr(credentials->password, '\0',
                   credentials->password_length) != NULL)
    {
        return false;
    }
    if (credentials->password_length >= 8U &&
            credentials->password_length <= 63U)
    {
        return true;
    }
    return credentials->password_length == WIFI_SERVICE_PASSWORD_MAX_BYTES &&
           _wifi_service_ascii_hex(credentials->password,
                                   credentials->password_length);
}

/** @file Error-name helper for event-bus host tests. */

#include "esp_err.h"

const char *esp_err_to_name(esp_err_t error)
{
    switch (error)
    {
    case ESP_OK:
        return "ESP_OK";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    default:
        return "ESP_ERR_UNKNOWN";
    }
}

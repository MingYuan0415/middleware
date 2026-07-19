#ifndef __POWER_SERVICE_HOST_ESP_ERR_H__
#define __POWER_SERVICE_HOST_ESP_ERR_H__

typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT       0x105

#endif /* __POWER_SERVICE_HOST_ESP_ERR_H__ */

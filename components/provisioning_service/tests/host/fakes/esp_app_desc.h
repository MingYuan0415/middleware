#ifndef __PROVISIONING_HOST_ESP_APP_DESC_H__
#define __PROVISIONING_HOST_ESP_APP_DESC_H__

typedef struct esp_app_desc
{
    char version[32];
} esp_app_desc_t;

const esp_app_desc_t *esp_app_get_description(void);

#endif /* __PROVISIONING_HOST_ESP_APP_DESC_H__ */

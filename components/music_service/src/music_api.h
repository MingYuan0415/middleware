#ifndef __MUSIC_API_H__
#define __MUSIC_API_H__

#include "music_service_internal.h"

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

cJSON *music_api_post(const char *module, const char *method,
                      cJSON *param, cJSON **root_out);
cJSON *music_api_post_with_credentials(const char *uin, const char *musickey,
                                       const char *module, const char *method,
                                       cJSON *param, cJSON **root_out);
esp_err_t music_api_last_error(void);
void music_json_string(const cJSON *object, const char *key,
                       char *output, size_t output_size);
void music_json_string_or_number(const cJSON *object, const char *key,
                                 char *output, size_t output_size);
uint64_t music_json_u64(const cJSON *object, const char *key);
uint32_t music_json_u32(const cJSON *object, const char *key);
bool music_json_bool(const cJSON *object, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* __MUSIC_API_H__ */

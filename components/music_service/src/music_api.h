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

typedef enum
{
    MUSIC_API_SCOPE_PUBLIC = 0,
    MUSIC_API_SCOPE_AUTHENTICATED,
} music_api_scope_t;

typedef struct music_api_result
{
    esp_err_t error;
    int business_code;
    uint32_t credential_generation;
    bool has_business_code;
    bool credential_attached;
    cJSON *root;
    cJSON *data;
} music_api_result_t;

/**
 * @brief Send one QQ Music API request.
 *
 * The parameter object is consumed on every path. Authenticated requests copy
 * the current validated credential and require `credential_generation` to
 * match it. Authentication business codes are mapped only for these requests.
 */
esp_err_t music_api_post(music_api_scope_t scope,
                         uint32_t credential_generation,
                         const char *module, const char *method,
                         cJSON *param, music_api_result_t *result);
void music_api_result_release(music_api_result_t *result);
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

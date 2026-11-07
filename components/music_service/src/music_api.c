#define DBG_TAG "music_api"
#include "mt_log.h"

#include "music_service_internal.h"
#include "music_api.h"
#include "music_http.h"
#include "music_login_protocol.h"

#include "cJSON.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUSIC_JSON_MAX_BYTES CONFIG_MUSIC_SERVICE_MAX_RESPONSE_BYTES

void music_json_string(const cJSON *object, const char *key,
                       char *output, size_t output_size)
{
    output[0] = '\0';
    if (object == NULL)
    {
        return;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        (void)snprintf(output, output_size, "%s", item->valuestring);
    }
}

void music_json_string_or_number(const cJSON *object, const char *key,
                                 char *output, size_t output_size)
{
    output[0] = '\0';
    if (object == NULL)
    {
        return;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        (void)snprintf(output, output_size, "%s", item->valuestring);
    }
    else if (cJSON_IsNumber(item))
    {
        (void)snprintf(output, output_size, "%.0f", item->valuedouble);
    }
}

uint64_t music_json_u64(const cJSON *object, const char *key)
{
    if (object == NULL)
    {
        return 0U;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item))
    {
        return (uint64_t)item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        return strtoull(item->valuestring, NULL, 10);
    }
    return 0U;
}

uint32_t music_json_u32(const cJSON *object, const char *key)
{
    return (uint32_t)music_json_u64(object, key);
}

bool music_json_bool(const cJSON *object, const char *key)
{
    const cJSON *item = object == NULL ? NULL :
                        cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint != 0);
}

static bool _music_api_business_code(const cJSON *item, int *output)
{
    if (cJSON_IsNumber(item))
    {
        *output = item->valueint;
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return false;
    }
    char *end = NULL;
    const long value = strtol(item->valuestring, &end, 10);
    if (end == item->valuestring || end == NULL || *end != '\0' ||
            value < INT32_MIN || value > INT32_MAX)
    {
        return false;
    }
    *output = (int)value;
    return true;
}

static esp_err_t _music_api_copy_credential(
    uint32_t credential_generation,
    char musicid[sizeof(s_music.credential.musicid)],
    char musickey[sizeof(s_music.credential.musickey)])
{
    if (credential_generation == 0U || music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool current =
        s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED &&
        s_music.credential_generation == credential_generation;
    if (current)
    {
        (void)snprintf(musicid, sizeof(s_music.credential.musicid), "%s",
                       s_music.credential.musicid);
        (void)snprintf(musickey, sizeof(s_music.credential.musickey), "%s",
                       s_music.credential.musickey);
    }
    music_service_unlock();
    return current ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t music_api_post(music_api_scope_t scope,
                         uint32_t credential_generation,
                         const char *module, const char *method,
                         cJSON *param, music_api_result_t *result)
{
    if (result == NULL)
    {
        cJSON_Delete(param);
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->error = ESP_OK;
    result->business_code = -1;
    result->credential_generation = credential_generation;
    if (module == NULL || method == NULL || param == NULL ||
            (scope != MUSIC_API_SCOPE_PUBLIC &&
             scope != MUSIC_API_SCOPE_AUTHENTICATED))
    {
        cJSON_Delete(param);
        result->error = ESP_ERR_INVALID_ARG;
        return result->error;
    }

    char musicid[sizeof(s_music.credential.musicid)] = {0};
    char musickey[sizeof(s_music.credential.musickey)] = {0};
    result->credential_attached = scope == MUSIC_API_SCOPE_AUTHENTICATED;
    if (result->credential_attached)
    {
        result->error = _music_api_copy_credential(
                            credential_generation, musicid, musickey);
        if (result->error != ESP_OK)
        {
            cJSON_Delete(param);
            return result->error;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *comm = cJSON_CreateObject();
    cJSON *request = cJSON_CreateObject();
    if (root == NULL || comm == NULL || request == NULL)
    {
        cJSON_Delete(param);
        cJSON_Delete(root);
        cJSON_Delete(comm);
        cJSON_Delete(request);
        result->error = ESP_ERR_NO_MEM;
        return result->error;
    }
    cJSON_AddStringToObject(comm, "uin", result->credential_attached ?
                            musicid : "0");
    cJSON_AddStringToObject(comm, "format", "json");
    cJSON_AddNumberToObject(comm, "ct", 24);
    cJSON_AddNumberToObject(comm, "cv", 0);
    cJSON_AddStringToObject(comm, "platform", "yqq.json");
    if (strcmp(module, "music.login.LoginServer") == 0 &&
            strcmp(method, "Login") == 0)
    {
        cJSON_AddNumberToObject(comm, "tmeLoginType", 1);
    }
    else if (strcmp(module, "QQConnectLogin.LoginServer") == 0 &&
             strcmp(method, "QQLogin") == 0)
    {
        cJSON_AddStringToObject(comm, "tmeLoginType", "2");
    }
    if (result->credential_attached)
    {
        char g_tk[24];
        (void)snprintf(g_tk, sizeof(g_tk), "%u",
                       (unsigned)music_login_hash33(musickey, 5381U));
        cJSON_AddStringToObject(comm, "g_tk", g_tk);
        cJSON_AddStringToObject(comm, "authst", musickey);
    }
    cJSON_AddItemToObject(root, "comm", comm);
    cJSON_AddStringToObject(request, "module", module);
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddItemToObject(request, "param", param);
    cJSON_AddItemToObject(root, "req_0", request);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        result->error = ESP_ERR_NO_MEM;
        return result->error;
    }
    music_http_result_t http;
    result->error = music_http_post(
                        "https://u.y.qq.com/cgi-bin/musicu.fcg", body, &http,
                        MUSIC_JSON_MAX_BYTES);
    cJSON_free(body);
    if (result->error != ESP_OK)
    {
        music_http_result_free(&http);
        return result->error;
    }
    if (http.status == 401 ||
            (http.status == 403 && result->credential_attached))
    {
        result->error = MUSIC_ERR_HTTP_AUTH;
        music_http_result_free(&http);
        return result->error;
    }
    if (http.status == 429)
    {
        result->error = MUSIC_ERR_HTTP_RATE_LIMIT;
        music_http_result_free(&http);
        return result->error;
    }
    if (http.status < 200 || http.status >= 300)
    {
        result->error = ESP_ERR_INVALID_RESPONSE;
        music_http_result_free(&http);
        return result->error;
    }
    result->root = cJSON_Parse(http.body);
    music_http_result_free(&http);
    if (result->root == NULL)
    {
        result->error = ESP_ERR_INVALID_RESPONSE;
        return result->error;
    }
    cJSON *response_request = cJSON_GetObjectItemCaseSensitive(result->root,
        "req_0");
    const cJSON *response_code = response_request == NULL ? NULL :
                                 cJSON_GetObjectItemCaseSensitive(
                                     response_request, "code");
    result->has_business_code = _music_api_business_code(
                                    response_code, &result->business_code);
    if (result->credential_attached && result->has_business_code &&
            music_login_code_is_auth_expired(result->business_code))
    {
        LOG_W("%s/%s auth expired: code=%d", module, method,
              result->business_code);
        result->error = MUSIC_ERR_HTTP_AUTH;
        music_api_result_release(result);
        return result->error;
    }
    result->data = response_request == NULL ? NULL :
                   cJSON_GetObjectItemCaseSensitive(response_request, "data");
    if (result->data == NULL)
    {
        LOG_W("%s/%s failed: code=%d", module, method,
              result->has_business_code ? result->business_code : -1);
        result->error = ESP_ERR_INVALID_RESPONSE;
        music_api_result_release(result);
        return result->error;
    }
    return ESP_OK;
}

void music_api_result_release(music_api_result_t *result)
{
    if (result == NULL)
    {
        return;
    }
    cJSON_Delete(result->root);
    result->root = NULL;
    result->data = NULL;
}


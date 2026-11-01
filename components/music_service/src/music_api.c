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
#include <string.h>

#define MUSIC_JSON_MAX_BYTES CONFIG_MUSIC_SERVICE_MAX_RESPONSE_BYTES

static _Thread_local esp_err_t s_last_musicu_error = ESP_OK;

esp_err_t music_api_last_error(void)
{
    return s_last_musicu_error;
}

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

cJSON *music_api_post(const char *module, const char *method,
                      cJSON *param, cJSON **root_out)
{
    *root_out = NULL;
    s_last_musicu_error = ESP_OK;
    if (module == NULL || method == NULL || param == NULL)
    {
        s_last_musicu_error = ESP_ERR_INVALID_ARG;
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *comm = cJSON_CreateObject();
    cJSON *request = cJSON_CreateObject();
    if (root == NULL || comm == NULL || request == NULL)
    {
        cJSON_Delete(root);
        cJSON_Delete(comm);
        cJSON_Delete(request);
        s_last_musicu_error = ESP_ERR_NO_MEM;
        return NULL;
    }
    const bool logged_in = s_music.credential_valid;
    cJSON_AddStringToObject(comm, "uin",
                            logged_in ? s_music.credential.musicid : "0");
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
    if (logged_in)
    {
        char g_tk[24];
        (void)snprintf(g_tk, sizeof(g_tk), "%u",
                       (unsigned)music_login_hash33(
                           s_music.credential.musickey, 5381U));
        cJSON_AddStringToObject(comm, "g_tk", g_tk);
        cJSON_AddStringToObject(comm, "authst", s_music.credential.musickey);
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
        s_last_musicu_error = ESP_ERR_NO_MEM;
        return NULL;
    }
    music_http_result_t result;
    const esp_err_t error = music_http_post(
                                "https://u.y.qq.com/cgi-bin/musicu.fcg", body, &result,
                                MUSIC_JSON_MAX_BYTES);
    cJSON_free(body);
    if (error != ESP_OK)
    {
        s_last_musicu_error = error;
        music_http_result_free(&result);
        return NULL;
    }
    if (result.status == 401 || result.status == 403)
    {
        s_last_musicu_error = MUSIC_ERR_HTTP_AUTH;
        music_http_result_free(&result);
        return NULL;
    }
    if (result.status == 429)
    {
        s_last_musicu_error = MUSIC_ERR_HTTP_RATE_LIMIT;
        music_http_result_free(&result);
        return NULL;
    }
    if (result.status >= 400)
    {
        s_last_musicu_error = ESP_ERR_INVALID_RESPONSE;
        music_http_result_free(&result);
        return NULL;
    }
    cJSON *response = cJSON_Parse(result.body);
    music_http_result_free(&result);
    if (response == NULL)
    {
        s_last_musicu_error = ESP_ERR_INVALID_RESPONSE;
        return NULL;
    }
    cJSON *response_request = cJSON_GetObjectItemCaseSensitive(response,
        "req_0");
    cJSON *data = response_request == NULL ? NULL :
                  cJSON_GetObjectItemCaseSensitive(response_request, "data");
    if (data == NULL)
    {
        const cJSON *code = response_request == NULL ? NULL :
                            cJSON_GetObjectItemCaseSensitive(response_request,
                                "code");
        LOG_W("%s/%s failed: code=%d", module, method,
              cJSON_IsNumber(code) ? code->valueint : -1);
        cJSON_Delete(response);
        s_last_musicu_error = ESP_ERR_INVALID_RESPONSE;
        return NULL;
    }
    *root_out = response;
    return data;
}


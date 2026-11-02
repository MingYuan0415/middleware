#ifndef __MUSIC_HTTP_H__
#define __MUSIC_HTTP_H__

#include "music_service_internal.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t music_http_init(void);
void music_http_deinit(void);
bool music_http_cookie_get(const char *name, char *value, size_t value_size);
void music_http_cookie_clear(void);
esp_err_t music_http_get_referer(const char *url, bool with_cookie,
                                 const char *referer,
                                 music_http_result_t *result, size_t max_body);
esp_err_t music_http_get(const char *url, bool with_cookie,
                         music_http_result_t *result, size_t max_body);
esp_err_t music_http_get_headers(const char *url, bool with_cookie,
                                 const char *referer,
                                 music_http_result_t *result);
esp_err_t music_http_post(const char *url, const char *body,
                          music_http_result_t *result, size_t max_body);
esp_err_t music_http_post_form_no_redirect(const char *url,
        const char *body, const char *referer,
        music_http_result_t *result, size_t max_body);
void music_http_result_free(music_http_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* __MUSIC_HTTP_H__ */

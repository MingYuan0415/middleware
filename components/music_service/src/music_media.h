#ifndef __MUSIC_MEDIA_H__
#define __MUSIC_MEDIA_H__

#include "music_service_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t music_media_fetch_image(const char *url, uint16_t width,
                                  uint16_t height, uint32_t token,
                                  bool with_cookie, const char *referer,
                                  music_image_t **image);


#ifdef __cplusplus
}
#endif

#endif /* __MUSIC_MEDIA_H__ */

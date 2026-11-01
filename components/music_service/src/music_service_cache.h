#ifndef __MUSIC_SERVICE_CACHE_H__
#define __MUSIC_SERVICE_CACHE_H__

#include "music_service.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether a cached playlist page satisfies an acquire request.
 *
 * A non-empty response is complete for caching purposes even when it is shorter
 * than the requested page: the snapshot total distinguishes a final page from a
 * partial response, and the App renders both without issuing an endless retry.
 */
bool music_service_playlist_cache_matches(
    const music_playlist_snapshot_t *cached, const music_playlist_ref_t *ref,
    uint16_t offset, uint16_t count);

/**
 * @brief Whether a cached cover satisfies an image acquire request.
 *
 * The rendered size is part of the identity because the decoder produces a
 * scaled bitmap.  A cover requested for a 96 px player must not satisfy a
 * later 64 px list row that shares the same URL and token.
 */
bool music_service_image_cache_matches(
    const music_image_t *cached, const char *url, uint16_t width,
    uint16_t height, uint32_t token);

#ifdef __cplusplus
}
#endif

#endif /* __MUSIC_SERVICE_CACHE_H__ */

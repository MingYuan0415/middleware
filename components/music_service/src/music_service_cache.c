#include "music_service_cache.h"

#include <string.h>

bool music_service_playlist_cache_matches(
    const music_playlist_snapshot_t *cached, const music_playlist_ref_t *ref,
    uint16_t offset, uint16_t count)
{
    if (cached == NULL || ref == NULL || count == 0U)
    {
        return false;
    }
    return cached->playlist.ref.id == ref->id &&
           cached->playlist.ref.dirid == ref->dirid &&
           cached->playlist.ref.tid == ref->tid &&
           cached->playlist.ref.kind == ref->kind &&
           cached->offset == offset &&
           cached->count != 0U;
}

bool music_service_image_cache_matches(
    const music_image_t *cached, const char *url, uint16_t width,
    uint16_t height, uint32_t token)
{
    if (cached == NULL || url == NULL || url[0] == '\0' ||
            width == 0U || height == 0U)
    {
        return false;
    }
    return cached->token == token &&
           cached->requested_width == width &&
           cached->requested_height == height &&
           strcmp(cached->url, url) == 0;
}

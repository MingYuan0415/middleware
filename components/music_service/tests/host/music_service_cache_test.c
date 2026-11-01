#include "music_service_cache.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static music_playlist_snapshot_t _snapshot(
    music_playlist_ref_t ref, uint16_t offset, uint16_t count,
    uint32_t total)
{
    music_playlist_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.playlist.ref = ref;
    snapshot.offset = offset;
    snapshot.count = count;
    snapshot.total = total;
    return snapshot;
}

int main(void)
{
    const music_playlist_ref_t ref =
    {
        .id = 7U,
        .dirid = 11U,
        .tid = 13U,
        .kind = 19U,
    };

    music_playlist_snapshot_t cached = _snapshot(ref, 0U, 30U, 30U);
    assert(music_service_playlist_cache_matches(
               &cached, &ref, 0U, 30U));

    cached = _snapshot(ref, 0U, 2U, 12U);
    assert(music_service_playlist_cache_matches(
               &cached, &ref, 0U, 30U));

    cached = _snapshot(ref, 30U, 5U, 35U);
    assert(music_service_playlist_cache_matches(
               &cached, &ref, 30U, 30U));

    cached = _snapshot(ref, 0U, 2U, 0U);
    assert(music_service_playlist_cache_matches(
               &cached, &ref, 0U, 30U));

    cached = _snapshot(ref, 30U, 2U, 12U);
    assert(!music_service_playlist_cache_matches(
               &cached, &ref, 0U, 30U));

    cached = _snapshot(ref, 0U, 0U, 12U);
    assert(!music_service_playlist_cache_matches(
               &cached, &ref, 0U, 30U));

    music_playlist_ref_t other = ref;
    other.tid = 14U;
    cached = _snapshot(ref, 0U, 2U, 12U);
    assert(!music_service_playlist_cache_matches(
               &cached, &other, 0U, 30U));

    assert(!music_service_playlist_cache_matches(NULL, &ref, 0U, 30U));
    cached = _snapshot(ref, 0U, 2U, 12U);
    assert(!music_service_playlist_cache_matches(
               &cached, NULL, 0U, 30U));

    music_image_t image;
    memset(&image, 0, sizeof(image));
    image.token = 0x1234U;
    image.requested_width = 96U;
    image.requested_height = 96U;
    (void)snprintf(image.url, sizeof(image.url),
                   "https://host.music.local/covers/1.jpg");
    assert(music_service_image_cache_matches(
               &image, image.url, 96U, 96U, 0x1234U));
    assert(!music_service_image_cache_matches(
               &image, image.url, 64U, 96U, 0x1234U));
    assert(!music_service_image_cache_matches(
               &image, image.url, 96U, 64U, 0x1234U));
    assert(!music_service_image_cache_matches(
               &image, image.url, 96U, 96U, 0x1235U));
    assert(!music_service_image_cache_matches(
               &image, "https://host.music.local/covers/2.jpg",
               96U, 96U, 0x1234U));
    assert(!music_service_image_cache_matches(NULL, image.url,
            96U, 96U, 0x1234U));
    assert(!music_service_image_cache_matches(&image, NULL,
            96U, 96U, 0x1234U));
    assert(!music_service_image_cache_matches(&image, "",
            96U, 96U, 0x1234U));
    assert(!music_service_image_cache_matches(&image, image.url,
            0U, 96U, 0x1234U));
    assert(!music_service_image_cache_matches(&image, image.url,
            96U, 0U, 0x1234U));

    printf("music_service_cache_test: PASS\n");
    return 0;
}

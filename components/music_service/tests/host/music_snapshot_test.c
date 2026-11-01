#include "music_snapshot.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal host heap caps implementation for the pure snapshot unit test. */
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(count, size);
}

void heap_caps_free(void *memory)
{
    free(memory);
}

typedef struct test_payload
{
    uint32_t marker;
    uint32_t destroyed;
} test_payload_t;

static void _destroy(void *payload)
{
    test_payload_t *item = payload;
    item->destroyed = 0xFFFFFFFFU;
}

int main(void)
{
    test_payload_t *item = music_snapshot_alloc(sizeof(*item), _destroy);
    assert(item != NULL);
    assert(item->destroyed == 0U);
    assert(music_snapshot_refcount(item) == 1U);

    assert(music_snapshot_retain(item) == item);
    assert(music_snapshot_refcount(item) == 2U);

    music_snapshot_release(item);
    assert(music_snapshot_refcount(item) == 1U);
    music_snapshot_release(item);

    /* A fresh allocation must be zeroed and independently releasable. */
    test_payload_t *second = music_snapshot_alloc(sizeof(*second), _destroy);
    assert(second != NULL);
    assert(second->marker == 0U);
    music_snapshot_release(second);

    printf("music_snapshot_test: PASS\n");
    return 0;
}

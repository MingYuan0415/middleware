#include "music_snapshot.h"

#include "esp_heap_caps.h"

#include <stdatomic.h>
#include <stdbool.h>

#define MUSIC_SNAPSHOT_MAGIC 0x4D534E50U /* "MSNP" */

typedef struct music_snapshot_ref
{
    _Atomic uint32_t refcount;
    uint32_t magic;
    music_snapshot_destroy_fn destroy;
    uint32_t reserved;
} music_snapshot_ref_t;

static music_snapshot_ref_t *_snapshot_ref(const void *payload)
{
    if (payload == NULL)
    {
        return NULL;
    }
    music_snapshot_ref_t *ref =
        ((music_snapshot_ref_t *)payload) - 1;
    return ref->magic == MUSIC_SNAPSHOT_MAGIC ? ref : NULL;
}

void *music_snapshot_alloc(size_t payload_bytes,
                           music_snapshot_destroy_fn destroy)
{
    if (payload_bytes == 0U)
    {
        return NULL;
    }
    const size_t total = sizeof(music_snapshot_ref_t) + payload_bytes;
    music_snapshot_ref_t *ref = heap_caps_calloc(
                                    1U, total,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ref == NULL)
    {
        return NULL;
    }
    ref->magic = MUSIC_SNAPSHOT_MAGIC;
    ref->destroy = destroy;
    atomic_store_explicit(&ref->refcount, 1U, memory_order_relaxed);
    return (void *)(ref + 1);
}

void *music_snapshot_retain(void *payload)
{
    music_snapshot_ref_t *ref = _snapshot_ref(payload);
    if (ref == NULL)
    {
        return NULL;
    }
    (void)atomic_fetch_add_explicit(&ref->refcount, 1U,
                                    memory_order_relaxed);
    return payload;
}

void music_snapshot_release(void *payload)
{
    music_snapshot_ref_t *ref = _snapshot_ref(payload);
    if (ref == NULL)
    {
        return;
    }
    const uint32_t previous = atomic_fetch_sub_explicit(
                                  &ref->refcount, 1U,
                                  memory_order_acq_rel);
    if (previous == 1U)
    {
        if (ref->destroy != NULL)
        {
            ref->destroy(payload);
        }
        ref->magic = 0U;
        heap_caps_free(ref);
    }
}

uint32_t music_snapshot_refcount(const void *payload)
{
    const music_snapshot_ref_t *ref = _snapshot_ref(payload);
    return ref == NULL ? 0U : atomic_load_explicit(
               &ref->refcount, memory_order_relaxed);
}

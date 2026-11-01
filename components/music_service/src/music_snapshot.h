#ifndef __MUSIC_SNAPSHOT_H__
#define __MUSIC_SNAPSHOT_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Destructor run when the last reference to a payload is dropped. */
typedef void (*music_snapshot_destroy_fn)(void *payload);

/**
 * @brief Allocate one reference-counted payload in PSRAM.
 *
 * The returned pointer addresses the payload, not the hidden reference header.
 * The payload is zero-initialized. Callers own exactly one reference.
 *
 * @param payload_bytes size of the payload in bytes.
 * @param destroy optional destructor for resources owned by the payload.
 * @return payload pointer, or NULL on allocation failure.
 */
void *music_snapshot_alloc(size_t payload_bytes,
                           music_snapshot_destroy_fn destroy);

/** @brief Add one reference to a payload. NULL is accepted and returned. */
void *music_snapshot_retain(void *payload);

/** @brief Drop one reference; release the payload at zero. NULL is accepted. */
void music_snapshot_release(void *payload);

/** @brief Return the current reference count, or 0 for NULL/invalid payload. */
uint32_t music_snapshot_refcount(const void *payload);

#ifdef __cplusplus
}
#endif

#endif /* __MUSIC_SNAPSHOT_H__ */

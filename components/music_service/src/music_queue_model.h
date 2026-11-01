#ifndef __MUSIC_QUEUE_MODEL_H__
#define __MUSIC_QUEUE_MODEL_H__

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Result of removing one queue entry. */
typedef struct music_queue_transition
{
    uint16_t count;   /**< Queue length after the removal. */
    uint16_t index;   /**< Current index after the removal. */
    bool restart;     /**< The replacement at index must start playing. */
} music_queue_transition_t;

/**
 * @brief Compute the queue state after removing one entry.
 *
 * @param count is the current queue length.
 * @param current is the current playback index.
 * @param removed is the index to remove.
 * @param result receives the transition.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG for an empty/out-of-range queue.
 */
esp_err_t music_queue_after_remove(uint16_t count, uint16_t current,
                                   uint16_t removed,
                                   music_queue_transition_t *result);

#endif /* __MUSIC_QUEUE_MODEL_H__ */

#ifndef __MUSIC_QUEUE_MODEL_H__
#define __MUSIC_QUEUE_MODEL_H__

#include "esp_err.h"
#include "music_service.h"

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

/**
 * @brief Compute a bounded window start that contains one queue index.
 *
 * The window prefers one page of context before the focused index and clamps
 * itself to the queue tail so the returned window never exceeds the queue.
 *
 * @param index is the logical queue index that must be covered.
 * @param total is the logical queue length.
 * @param window_size is the maximum number of entries in one window.
 * @param page_size is the provider page size used to keep the window aligned.
 * @return Window start index, or 0 for invalid/empty input.
 */
uint16_t music_queue_window_start(uint16_t index, uint16_t total,
                                  uint16_t window_size, uint16_t page_size);

/** @brief Whether [offset, offset + count) contains one queue index. */
bool music_queue_window_contains(uint16_t offset, uint16_t count,
                                 uint16_t index);

/**
 * @brief Number of entries available from one window offset.
 *
 * @param total is the logical queue length.
 * @param offset is the first logical index of the window.
 * @param window_size is the maximum number of entries in one window.
 * @return Entry count clamped to the queue tail, or 0 for invalid input.
 */
uint16_t music_queue_window_count(uint16_t total, uint16_t offset,
                                  uint16_t window_size);

/**
 * @brief Pick the next queue entry after the current one cannot play.
 *
 * Sequential playback stops at the queue end; repeat-all wraps; repeat-one
 * advances instead of retrying the unavailable entry; shuffle avoids the
 * current entry when more than one entry exists.
 *
 * @param count is the logical queue length.
 * @param current is the index that failed.
 * @param mode is the active play mode.
 * @param random_value seeds shuffle selection.
 * @param next receives the next index.
 * @return true when another entry can be tried.
 */
bool music_queue_next_after_failure(uint16_t count, uint16_t current,
                                    music_play_mode_t mode,
                                    uint32_t random_value, uint16_t *next);

/** @brief Number of provider pages needed for a queue length. */
uint16_t music_queue_page_count(uint16_t total, uint16_t page_size);

/** @brief Provider page containing one logical queue index. */
uint16_t music_queue_page_for_index(uint16_t index, uint16_t page_size);

/**
 * @brief Whether one source playlist index has been removed from the queue.
 *
 * @param removed is the removal bitmap, least-significant bit first.
 * @param source_total is the original playlist length.
 * @param source_index is the source playlist index.
 */
bool music_queue_source_is_removed(const uint8_t *removed,
                                   uint16_t source_total,
                                   uint16_t source_index);

/**
 * @brief Map a logical queue index to its source playlist index.
 *
 * Removed source entries are skipped. Returns false when the logical index is
 * outside the remaining queue.
 *
 * @param source_total is the original playlist length.
 * @param removed is the removal bitmap.
 * @param logical is the logical queue index.
 * @param source receives the source playlist index.
 */
bool music_queue_logical_to_source(uint16_t source_total,
                                   const uint8_t *removed,
                                   uint16_t logical,
                                   uint16_t *source);

/**
 * @brief Map a source playlist index to its logical queue index.
 *
 * Returns false when the source entry is removed or outside the source range.
 *
 * @param source_total is the original playlist length.
 * @param removed is the removal bitmap.
 * @param source is the source playlist index.
 * @param logical receives the logical queue index.
 */
bool music_queue_source_to_logical(uint16_t source_total,
                                   const uint8_t *removed,
                                   uint16_t source,
                                   uint16_t *logical);

#endif /* __MUSIC_QUEUE_MODEL_H__ */

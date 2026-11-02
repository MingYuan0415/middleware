#include "music_queue_model.h"

esp_err_t music_queue_after_remove(uint16_t count, uint16_t current,
                                   uint16_t removed,
                                   music_queue_transition_t *result)
{
    if (result == NULL || count == 0U || current >= count ||
            removed >= count)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t remaining = (uint16_t)(count - 1U);
    result->count = remaining;
    result->restart = false;
    if (remaining == 0U)
    {
        result->index = 0U;
        return ESP_OK;
    }
    if (removed < current)
    {
        result->index = (uint16_t)(current - 1U);
    }
    else if (removed > current)
    {
        result->index = current;
    }
    else
    {
        result->index = current < remaining ? current :
                        (uint16_t)(remaining - 1U);
        result->restart = true;
    }
    return ESP_OK;
}

uint16_t music_queue_window_start(uint16_t index, uint16_t total,
                                  uint16_t window_size, uint16_t page_size)
{
    if (index >= total || window_size == 0U || page_size == 0U)
    {
        return 0U;
    }
    if (total <= window_size)
    {
        return 0U;
    }
    uint32_t start = index > window_size / 2U ?
                     (uint32_t)index - window_size / 2U : 0U;
    if (start + window_size > total)
    {
        start = (uint32_t)total - window_size;
    }
    else
    {
        start -= start % page_size;
    }
    return (uint16_t)start;
}

bool music_queue_window_contains(uint16_t offset, uint16_t count,
                                 uint16_t index)
{
    return count != 0U && index >= offset &&
           (uint32_t)index - offset < count;
}

uint16_t music_queue_window_count(uint16_t total, uint16_t offset,
                                  uint16_t window_size)
{
    if (offset >= total || window_size == 0U)
    {
        return 0U;
    }
    const uint16_t remaining = (uint16_t)(total - offset);
    return remaining > window_size ? window_size : remaining;
}

bool music_queue_next_after_failure(uint16_t count, uint16_t current,
                                    music_play_mode_t mode,
                                    uint32_t random_value, uint16_t *next)
{
    if (count < 2U || current >= count || next == NULL)
    {
        return false;
    }
    uint16_t candidate = 0U;
    if (mode == MUSIC_PLAY_MODE_SHUFFLE)
    {
        candidate = (uint16_t)(random_value % count);
        if (candidate == current)
        {
            candidate = (uint16_t)((candidate + 1U) % count);
        }
    }
    else if (current + 1U < count)
    {
        candidate = (uint16_t)(current + 1U);
    }
    else if (mode == MUSIC_PLAY_MODE_REPEAT_ALL ||
             mode == MUSIC_PLAY_MODE_REPEAT_ONE)
    {
        candidate = 0U;
    }
    else
    {
        return false;
    }
    *next = candidate;
    return true;
}

uint16_t music_queue_page_count(uint16_t total, uint16_t page_size)
{
    if (total == 0U || page_size == 0U)
    {
        return 0U;
    }
    return (uint16_t)(((uint32_t)total + page_size - 1U) / page_size);
}

uint16_t music_queue_page_for_index(uint16_t index, uint16_t page_size)
{
    return page_size == 0U ? 0U : (uint16_t)(index / page_size);
}

bool music_queue_source_is_removed(const uint8_t *removed,
                                   uint16_t source_total,
                                   uint16_t source_index)
{
    if (removed == NULL || source_index >= source_total)
    {
        return false;
    }
    return (removed[source_index / 8U] &
            (uint8_t)(1U << (source_index % 8U))) != 0U;
}

static uint16_t _music_queue_removed_before(const uint8_t *removed,
        uint16_t source_total, uint16_t source_index)
{
    uint16_t count = 0U;
    const uint16_t limit = source_index < source_total ?
                           source_index : source_total;
    for (uint16_t index = 0U; index < limit; ++index)
    {
        if (music_queue_source_is_removed(removed, source_total, index))
        {
            ++count;
        }
    }
    return count;
}

bool music_queue_logical_to_source(uint16_t source_total,
                                   const uint8_t *removed,
                                   uint16_t logical,
                                   uint16_t *source)
{
    if (removed == NULL || source == NULL)
    {
        return false;
    }
    const uint16_t removed_total = _music_queue_removed_before(
                                       removed, source_total, source_total);
    if (logical >= (uint16_t)(source_total - removed_total))
    {
        return false;
    }
    uint16_t skipped = 0U;
    for (uint16_t index = 0U; index < source_total; ++index)
    {
        if (music_queue_source_is_removed(removed, source_total, index))
        {
            continue;
        }
        if (skipped == logical)
        {
            *source = index;
            return true;
        }
        ++skipped;
    }
    return false;
}

bool music_queue_source_to_logical(uint16_t source_total,
                                   const uint8_t *removed,
                                   uint16_t source,
                                   uint16_t *logical)
{
    if (removed == NULL || logical == NULL || source >= source_total ||
            music_queue_source_is_removed(removed, source_total, source))
    {
        return false;
    }
    *logical = (uint16_t)(source -
                          _music_queue_removed_before(removed, source_total,
                              source));
    return true;
}

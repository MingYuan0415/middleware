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

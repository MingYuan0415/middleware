#include <stdint.h>

#include "esp_random.h"

static uint32_t s_state = 0x12345678U;

uint32_t esp_random(void)
{
    /* xorshift32 deterministic sequence. */
    uint32_t x = s_state;

    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    s_state = x;
    return x;
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *bytes = (uint8_t *)buf;

    for (size_t i = 0U; i < len; ++i)
    {
        bytes[i] = (uint8_t)(esp_random() >> (8U * (i % 4U)));
    }
}

void esp_random_fake_reset(uint32_t seed)
{
    s_state = seed != 0U ? seed : 0x12345678U;
}

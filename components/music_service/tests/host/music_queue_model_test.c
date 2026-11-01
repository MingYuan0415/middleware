#include "music_queue_model.h"

#include <assert.h>
#include <stdio.h>

static void test_remove_before_current(void)
{
    music_queue_transition_t result;
    assert(music_queue_after_remove(5U, 3U, 1U, &result) == ESP_OK);
    assert(result.count == 4U);
    assert(result.index == 2U);
    assert(!result.restart);
}

static void test_remove_after_current(void)
{
    music_queue_transition_t result;
    assert(music_queue_after_remove(5U, 1U, 4U, &result) == ESP_OK);
    assert(result.count == 4U);
    assert(result.index == 1U);
    assert(!result.restart);
}

static void test_remove_current_middle(void)
{
    music_queue_transition_t result;
    assert(music_queue_after_remove(5U, 2U, 2U, &result) == ESP_OK);
    assert(result.count == 4U);
    assert(result.index == 2U);
    assert(result.restart);
}

static void test_remove_current_last(void)
{
    music_queue_transition_t result;
    assert(music_queue_after_remove(4U, 3U, 3U, &result) == ESP_OK);
    assert(result.count == 3U);
    assert(result.index == 2U);
    assert(result.restart);
}

static void test_remove_only_entry(void)
{
    music_queue_transition_t result;
    assert(music_queue_after_remove(1U, 0U, 0U, &result) == ESP_OK);
    assert(result.count == 0U);
    assert(result.index == 0U);
    assert(!result.restart);
}

static void test_invalid_input(void)
{
    music_queue_transition_t result;
    assert(music_queue_after_remove(0U, 0U, 0U, &result) ==
           ESP_ERR_INVALID_ARG);
    assert(music_queue_after_remove(2U, 2U, 0U, &result) ==
           ESP_ERR_INVALID_ARG);
    assert(music_queue_after_remove(2U, 0U, 2U, &result) ==
           ESP_ERR_INVALID_ARG);
    assert(music_queue_after_remove(2U, 0U, 0U, NULL) ==
           ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_remove_before_current();
    test_remove_after_current();
    test_remove_current_middle();
    test_remove_current_last();
    test_remove_only_entry();
    test_invalid_input();
    (void)printf("music_queue_model_test: ok\n");
    return 0;
}

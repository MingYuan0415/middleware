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

static void test_window_start(void)
{
    assert(music_queue_window_start(0U, 0U, 30U, 10U) == 0U);
    assert(music_queue_window_start(5U, 5U, 30U, 10U) == 0U);
    assert(music_queue_window_start(0U, 137U, 30U, 10U) == 0U);
    assert(music_queue_window_start(5U, 137U, 30U, 10U) == 0U);
    assert(music_queue_window_start(52U, 137U, 30U, 10U) == 30U);
    assert(music_queue_window_start(136U, 137U, 30U, 10U) == 107U);
    assert(music_queue_window_start(136U, 137U, 30U, 10U) + 30U == 137U);
    assert(music_queue_window_contains(30U, 30U, 52U));
    assert(!music_queue_window_contains(30U, 30U, 60U));
    assert(!music_queue_window_contains(30U, 0U, 30U));
}

static void test_page_math(void)
{
    assert(music_queue_page_count(0U, 20U) == 0U);
    assert(music_queue_page_count(1U, 20U) == 1U);
    assert(music_queue_page_count(20U, 20U) == 1U);
    assert(music_queue_page_count(21U, 20U) == 2U);
    assert(music_queue_page_count(137U, 20U) == 7U);
    assert(music_queue_page_for_index(0U, 20U) == 0U);
    assert(music_queue_page_for_index(19U, 20U) == 0U);
    assert(music_queue_page_for_index(20U, 20U) == 1U);
    assert(music_queue_page_for_index(136U, 20U) == 6U);
    assert(music_queue_page_count(137U, 0U) == 0U);
    assert(music_queue_page_for_index(137U, 0U) == 0U);
}

static void test_removal_mapping(void)
{
    uint8_t removed[1] = {0};
    removed[0] = (uint8_t)((1U << 2U) | (1U << 5U));
    assert(music_queue_source_is_removed(removed, 8U, 2U));
    assert(music_queue_source_is_removed(removed, 8U, 5U));
    assert(!music_queue_source_is_removed(removed, 8U, 4U));

    uint16_t source = UINT16_MAX;
    assert(music_queue_logical_to_source(8U, removed, 0U, &source));
    assert(source == 0U);
    assert(music_queue_logical_to_source(8U, removed, 1U, &source));
    assert(source == 1U);
    assert(music_queue_logical_to_source(8U, removed, 2U, &source));
    assert(source == 3U);
    assert(music_queue_logical_to_source(8U, removed, 3U, &source));
    assert(source == 4U);
    assert(music_queue_logical_to_source(8U, removed, 4U, &source));
    assert(source == 6U);
    assert(music_queue_logical_to_source(8U, removed, 5U, &source));
    assert(source == 7U);
    assert(!music_queue_logical_to_source(8U, removed, 6U, &source));
    assert(!music_queue_logical_to_source(8U, removed, 0U, NULL));

    uint16_t logical = UINT16_MAX;
    assert(music_queue_source_to_logical(8U, removed, 0U, &logical));
    assert(logical == 0U);
    assert(music_queue_source_to_logical(8U, removed, 3U, &logical));
    assert(logical == 2U);
    assert(!music_queue_source_to_logical(8U, removed, 2U, &logical));
    assert(!music_queue_source_to_logical(8U, removed, 8U, &logical));
    assert(!music_queue_source_to_logical(8U, removed, 0U, NULL));
}

int main(void)
{
    test_remove_before_current();
    test_remove_after_current();
    test_remove_current_middle();
    test_remove_current_last();
    test_remove_only_entry();
    test_invalid_input();
    test_window_start();
    test_page_math();
    test_removal_mapping();
    (void)printf("music_queue_model_test: ok\n");
    return 0;
}

#ifndef RECORDER_HOST_WORKER_H
#define RECORDER_HOST_WORKER_H

/* Host command execution uses a real thread and bounded queue. Streaming is
 * device-only; host recordings retain their open file until a stop command. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define RECORDER_TASK_CORE 0

typedef void *TaskHandle_t;
typedef struct recorder_host_queue
{
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    size_t item_size;
    size_t capacity;
    size_t count;
    size_t front;
    unsigned char data[];
} *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(size_t capacity, size_t item_size)
{
    QueueHandle_t queue = calloc(1U, sizeof(*queue) + capacity * item_size);
    if (queue != NULL)
    {
        pthread_mutex_init(&queue->mutex, NULL);
        pthread_cond_init(&queue->changed, NULL);
        queue->capacity = capacity;
        queue->item_size = item_size;
    }
    return queue;
}

static inline int xQueueSend(QueueHandle_t queue, const void *item, uint32_t timeout)
{
    (void)timeout;
    pthread_mutex_lock(&queue->mutex);
    if (queue->count == queue->capacity)
    {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }
    const size_t end = (queue->front + queue->count) % queue->capacity;
    memcpy(queue->data + end * queue->item_size, item, queue->item_size);
    ++queue->count;
    pthread_cond_signal(&queue->changed);
    pthread_mutex_unlock(&queue->mutex);
    return pdTRUE;
}

static inline int xQueueReceive(QueueHandle_t queue, void *item, uint32_t timeout)
{
    (void)timeout;
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0U)
    {
        pthread_cond_wait(&queue->changed, &queue->mutex);
    }
    memcpy(item, queue->data + queue->front * queue->item_size, queue->item_size);
    queue->front = (queue->front + 1U) % queue->capacity;
    --queue->count;
    pthread_mutex_unlock(&queue->mutex);
    return pdTRUE;
}

static inline void vQueueDelete(QueueHandle_t queue)
{
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->changed);
    free(queue);
}

struct recorder_host_start
{
    void (*function)(void *);
    void *argument;
};

static inline void *_recorder_host_run(void *argument)
{
    const struct recorder_host_start start = *(struct recorder_host_start *)argument;
    free(argument);
    start.function(start.argument);
    return NULL;
}

static inline int xTaskCreatePinnedToCore(void (*function)(void *), const char *name,
        unsigned stack, void *argument, unsigned priority,
        TaskHandle_t *handle, unsigned core)
{
    (void)name;
    (void)stack;
    (void)priority;
    (void)core;
    struct recorder_host_start *start = malloc(sizeof(*start));
    if (start == NULL)
    {
        return 0;
    }
    *start = (struct recorder_host_start)
    {
        function, argument
    };
    *handle = (void *)1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, _recorder_host_run, start) != 0)
    {
        *handle = NULL;
        free(start);
        return 0;
    }
    pthread_detach(thread);
    return pdPASS;
}

static inline void vTaskDelay(uint32_t milliseconds)
{
    const struct timespec delay =
    {
        .tv_sec = milliseconds / 1000U,
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

static inline void vTaskDelete(TaskHandle_t handle)
{
    (void)handle;
    pthread_exit(NULL);
}

#endif

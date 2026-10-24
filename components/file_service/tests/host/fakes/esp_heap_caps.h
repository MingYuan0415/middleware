#ifndef __FILE_SERVICE_FAKE_ESP_HEAP_CAPS_H__
#define __FILE_SERVICE_FAKE_ESP_HEAP_CAPS_H__

/* Host tests use the process allocator; the simulator and firmware map these
 * onto the real capability heap. */
#include <stdlib.h>

#define MALLOC_CAP_8BIT   (1U << 2)
#define MALLOC_CAP_SPIRAM (1U << 10)
#define MALLOC_CAP_INTERNAL (1U << 11)

void *file_service_test_allocate(size_t size, unsigned caps);
void file_service_test_release(void *pointer);
#define heap_caps_malloc(size, caps) file_service_test_allocate(size, caps)
#define heap_caps_free(pointer) file_service_test_release(pointer)

#endif /* __FILE_SERVICE_FAKE_ESP_HEAP_CAPS_H__ */

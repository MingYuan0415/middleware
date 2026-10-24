#ifndef __FREERTOS_FREERTOS_H__
#define __FREERTOS_FREERTOS_H__

#include <assert.h>
#include <pthread.h>

typedef pthread_mutex_t portMUX_TYPE;
extern _Thread_local int sd_test_lock_depth;

#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(lock) do { assert(pthread_mutex_lock(lock) == 0); ++sd_test_lock_depth; } while (0)
#define taskEXIT_CRITICAL(lock) do { --sd_test_lock_depth; assert(pthread_mutex_unlock(lock) == 0); } while (0)

#endif /* __FREERTOS_FREERTOS_H__ */

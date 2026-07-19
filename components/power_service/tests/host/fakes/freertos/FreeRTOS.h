#ifndef __POWER_SERVICE_HOST_FREERTOS_H__
#define __POWER_SERVICE_HOST_FREERTOS_H__

#include <pthread.h>

#include "../../../../../time_service/tests/host/include/freertos/FreeRTOS.h"

typedef pthread_mutex_t portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(mux) ((void)pthread_mutex_lock(mux))
#define taskEXIT_CRITICAL(mux)  ((void)pthread_mutex_unlock(mux))
#define BIT2                    (UINT32_C(1) << 2)
#define BIT3                    (UINT32_C(1) << 3)

#endif /* __POWER_SERVICE_HOST_FREERTOS_H__ */

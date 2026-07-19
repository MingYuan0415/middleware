#ifndef __FREERTOS_FREERTOS_H__
#define __FREERTOS_FREERTOS_H__

typedef int portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock)  ((void)(lock))

#endif /* __FREERTOS_FREERTOS_H__ */

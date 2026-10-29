/** @file No-op project logging macros for event-bus host tests. */
#ifndef EVENT_BUS_HOST_MT_LOG_H
#define EVENT_BUS_HOST_MT_LOG_H

#define DBG_ERROR   1
#define DBG_WARN    2
#define DBG_INFO    3
#define DBG_DEBUG   4
#define DBG_VERBOSE 5

static inline void event_bus_host_log_sink(const char *format, ...)
{
    (void)format;
}

#define LOG_E(...) event_bus_host_log_sink(__VA_ARGS__)
#define LOG_W(...) event_bus_host_log_sink(__VA_ARGS__)
#define LOG_I(...) event_bus_host_log_sink(__VA_ARGS__)
#define LOG_D(...) event_bus_host_log_sink(__VA_ARGS__)
#define LOG_V(...) event_bus_host_log_sink(__VA_ARGS__)

#endif /* EVENT_BUS_HOST_MT_LOG_H */

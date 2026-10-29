/** @file Trace-enabled logging for the event-bus compile check. */
#ifndef EVENT_BUS_HOST_TRACE_MT_LOG_H
#define EVENT_BUS_HOST_TRACE_MT_LOG_H

#define DBG_ERROR   1
#define DBG_WARN    2
#define DBG_INFO    3
#define DBG_DEBUG   4
#define DBG_VERBOSE 5

extern void event_bus_host_trace_log(const char *format, ...);

#define LOG_E(...) ((void)0)
#define LOG_W(...) event_bus_host_trace_log(__VA_ARGS__)
#define LOG_I(...) event_bus_host_trace_log(__VA_ARGS__)
#define LOG_D(...) event_bus_host_trace_log(__VA_ARGS__)
#define LOG_V(...) ((void)0)

#endif /* EVENT_BUS_HOST_TRACE_MT_LOG_H */

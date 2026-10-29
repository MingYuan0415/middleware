/** @file Force CONFIG_EVENT_BUS_TRACE on for the compile-only target. */
#ifndef EVENT_BUS_HOST_TRACE_OVERRIDE_H
#define EVENT_BUS_HOST_TRACE_OVERRIDE_H

#include "../../../../tests/host/sdkconfig.h"

#undef CONFIG_EVENT_BUS_TRACE
#define CONFIG_EVENT_BUS_TRACE 1

#endif /* EVENT_BUS_HOST_TRACE_OVERRIDE_H */

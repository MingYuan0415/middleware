file(GLOB_RECURSE PROJECT_CJSON_SOURCES
    "${MT_ROOT}/main/*.c"
    "${MT_ROOT}/main/*.h"
    "${MT_ROOT}/layers/*.c"
    "${MT_ROOT}/layers/*.h"
    "${MT_ROOT}/tests/*.c"
    "${MT_ROOT}/tests/*.h")

set(OWNER "${MT_ROOT}/layers/middleware/components/weather_service/src/weather_service_parse.c")
set(CALL_COUNT 0)
foreach(SOURCE IN LISTS PROJECT_CJSON_SOURCES)
    file(READ "${SOURCE}" CONTENT)
    string(REGEX MATCHALL "cJSON_InitHooks[ \t\r\n]*\\(" CALLS "${CONTENT}")
    list(LENGTH CALLS SOURCE_CALL_COUNT)
    if(SOURCE_CALL_COUNT GREATER 0 AND NOT SOURCE STREQUAL OWNER)
        message(FATAL_ERROR "cJSON_InitHooks is owned by Weather Service: ${SOURCE}")
    endif()
    math(EXPR CALL_COUNT "${CALL_COUNT} + ${SOURCE_CALL_COUNT}")
endforeach()

if(NOT CALL_COUNT EQUAL 2)
    message(FATAL_ERROR "Expected only the platform hook branches, found ${CALL_COUNT} calls")
endif()

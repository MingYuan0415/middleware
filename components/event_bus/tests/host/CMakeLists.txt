cmake_minimum_required(VERSION 3.16)
project(event_bus_host_regression C)

enable_testing()
find_package(Threads REQUIRED)

set(EVENT_BUS_SANITIZER "none" CACHE STRING "none, address, or thread")
set_property(CACHE EVENT_BUS_SANITIZER PROPERTY STRINGS none address thread)
find_program(EVENT_BUS_SETARCH setarch)

get_filename_component(EVENT_BUS_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(MIDDLEWARE_COMPONENTS_DIR "${EVENT_BUS_DIR}/.." ABSOLUTE)
get_filename_component(MIDDLEWARE_DIR "${MIDDLEWARE_COMPONENTS_DIR}/.." ABSOLUTE)

add_executable(event_bus_test
    test_event_bus.c
    "${EVENT_BUS_DIR}/src/event_bus.c"
    "${EVENT_BUS_DIR}/src/event_bus_core.c"
    "${MIDDLEWARE_COMPONENTS_DIR}/time_service/tests/host/freertos_fake.c"
    fakes/esp_err.c
)

target_include_directories(event_bus_test PRIVATE
    fakes
    "${EVENT_BUS_DIR}/include"
    "${EVENT_BUS_DIR}/src"
    "${MIDDLEWARE_COMPONENTS_DIR}/time_service/tests/host/include"
)

target_compile_features(event_bus_test PRIVATE c_std_11)
target_compile_definitions(event_bus_test PRIVATE
    _POSIX_C_SOURCE=200809L
    HOST_TEST=1
)
target_compile_options(event_bus_test PRIVATE
    -include "${MIDDLEWARE_DIR}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG
)
target_link_libraries(event_bus_test PRIVATE Threads::Threads)

# Compile the bus with EVENT_BUS_TRACE enabled against a logging fake which
# evaluates its arguments. This catches trace-only build breaks which the
# no-op host logger would hide.
add_library(event_bus_trace_compile OBJECT
    "${EVENT_BUS_DIR}/src/event_bus.c"
    "${EVENT_BUS_DIR}/src/event_bus_core.c"
)
target_include_directories(event_bus_trace_compile PRIVATE
    trace_fakes
    fakes
    "${EVENT_BUS_DIR}/include"
    "${EVENT_BUS_DIR}/src"
    "${MIDDLEWARE_COMPONENTS_DIR}/time_service/tests/host/include"
)
target_compile_features(event_bus_trace_compile PRIVATE c_std_11)
target_compile_definitions(event_bus_trace_compile PRIVATE
    _POSIX_C_SOURCE=200809L
    HOST_TEST=1
)
target_compile_options(event_bus_trace_compile PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/trace_override.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG
)

if(EVENT_BUS_SANITIZER STREQUAL "address")
    target_compile_options(event_bus_test PRIVATE
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -fsanitize=address,undefined
    )
    target_link_options(event_bus_test PRIVATE
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -fsanitize=address,undefined
    )
elseif(EVENT_BUS_SANITIZER STREQUAL "thread")
    target_compile_options(event_bus_test PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=thread
    )
    target_link_options(event_bus_test PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=thread
    )
elseif(NOT EVENT_BUS_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported EVENT_BUS_SANITIZER=${EVENT_BUS_SANITIZER}")
endif()

if(EVENT_BUS_SANITIZER STREQUAL "thread" AND
        EVENT_BUS_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME event_bus_test COMMAND
        ${EVENT_BUS_SETARCH} x86_64 -R $<TARGET_FILE:event_bus_test>)
else()
    add_test(NAME event_bus_test COMMAND event_bus_test)
endif()
set_tests_properties(event_bus_test PROPERTIES TIMEOUT 30)

cmake_minimum_required(VERSION 3.16)
project(time_service_host_regression C)

enable_testing()
find_package(Threads REQUIRED)

set(TIME_SERVICE_SANITIZER "none" CACHE STRING "none, address, or thread")
set_property(CACHE TIME_SERVICE_SANITIZER PROPERTY STRINGS none address thread)
find_program(TIME_SERVICE_SETARCH setarch)

get_filename_component(TIME_SERVICE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

function(configure_time_service_test target)
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(${target} PRIVATE
        -include "${TIME_SERVICE_DIR}/../../tests/host/sdkconfig.h"
        -Wall
        -Wextra
        -Werror
        -Wpedantic
    )
    target_link_libraries(${target} PRIVATE Threads::Threads)

    if(TIME_SERVICE_SANITIZER STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
            -fsanitize=address,undefined
        )
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
            -fsanitize=address,undefined
        )
    elseif(TIME_SERVICE_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=thread
        )
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=thread
        )
    elseif(NOT TIME_SERVICE_SANITIZER STREQUAL "none")
        message(FATAL_ERROR
            "Unsupported TIME_SERVICE_SANITIZER=${TIME_SERVICE_SANITIZER}")
    endif()
endfunction()

add_executable(time_service_callback_gate_test
    test_time_service_callback_gate.c
    event_bus_fake.c
    freertos_fake.c
    time_service_port_fake.c
    "${TIME_SERVICE_DIR}/src/time_service.c"
    "${TIME_SERVICE_DIR}/src/time_service_core.c"
)
target_include_directories(time_service_callback_gate_test PRIVATE
    include
    "${TIME_SERVICE_DIR}/../event_bus/include"
    "${TIME_SERVICE_DIR}/include"
    "${TIME_SERVICE_DIR}/src"
)
configure_time_service_test(time_service_callback_gate_test)

add_executable(time_service_alarm_test
    test_time_service_alarm.c
    event_bus_fake.c
    freertos_fake.c
    time_service_port_fake.c
    "${TIME_SERVICE_DIR}/src/time_service.c"
    "${TIME_SERVICE_DIR}/src/time_service_core.c"
)
target_include_directories(time_service_alarm_test PRIVATE
    include
    "${TIME_SERVICE_DIR}/../event_bus/include"
    "${TIME_SERVICE_DIR}/include"
    "${TIME_SERVICE_DIR}/src"
)
configure_time_service_test(time_service_alarm_test)

add_executable(time_service_sleep_test
    test_time_service_sleep.c
    event_bus_fake.c
    freertos_fake.c
    time_service_port_fake.c
    "${TIME_SERVICE_DIR}/src/time_service.c"
    "${TIME_SERVICE_DIR}/src/time_service_core.c"
)
target_include_directories(time_service_sleep_test PRIVATE
    include
    "${TIME_SERVICE_DIR}/../event_bus/include"
    "${TIME_SERVICE_DIR}/include"
    "${TIME_SERVICE_DIR}/src"
)
configure_time_service_test(time_service_sleep_test)

add_executable(time_service_port_barrier_test
    test_time_service_port_barrier.c
    time_service_idf_fake.c
    "${TIME_SERVICE_DIR}/src/time_service_port.c"
)
target_include_directories(time_service_port_barrier_test PRIVATE
    include
    "${TIME_SERVICE_DIR}/src"
)
target_compile_definitions(time_service_port_barrier_test PRIVATE
    _DEFAULT_SOURCE
)
configure_time_service_test(time_service_port_barrier_test)

foreach(test_target IN ITEMS
        time_service_callback_gate_test
        time_service_alarm_test
        time_service_sleep_test
        time_service_port_barrier_test)
    if(TIME_SERVICE_SANITIZER STREQUAL "thread" AND
            TIME_SERVICE_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        add_test(NAME ${test_target} COMMAND
            ${TIME_SERVICE_SETARCH} x86_64 -R $<TARGET_FILE:${test_target}>)
    else()
        add_test(NAME ${test_target} COMMAND ${test_target})
    endif()
    set_tests_properties(${test_target} PROPERTIES TIMEOUT 10)
endforeach()

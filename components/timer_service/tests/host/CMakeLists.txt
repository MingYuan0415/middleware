cmake_minimum_required(VERSION 3.16)
project(timer_service_host_tests C)

enable_testing()

set(TIMER_SERVICE_SANITIZER "none" CACHE STRING "none, address, or thread")
set_property(CACHE TIMER_SERVICE_SANITIZER PROPERTY STRINGS none address thread)

add_executable(timer_service_test
    timer_service_test.c
    ../../src/timer_service.c
)
target_include_directories(timer_service_test PRIVATE include ../../include)
target_compile_features(timer_service_test PRIVATE c_std_11)
target_compile_options(timer_service_test PRIVATE
    -Wall -Wextra -Werror -Wpedantic -UNDEBUG)

if(TIMER_SERVICE_SANITIZER STREQUAL "address")
    target_compile_options(timer_service_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(timer_service_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
elseif(TIMER_SERVICE_SANITIZER STREQUAL "thread")
    target_compile_options(timer_service_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(timer_service_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT TIMER_SERVICE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unsupported sanitizer")
endif()

add_test(NAME timer_service COMMAND timer_service_test)

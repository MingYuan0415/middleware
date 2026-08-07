cmake_minimum_required(VERSION 3.16)
project(mt_device_link_security_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(DEVICE_LINK_SECURITY_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE DEVICE_LINK_SECURITY_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(DEVICE_LINK_SECURITY_SETARCH setarch)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(COMPONENT "${PROJECT_ROOT}/layers/middleware/components/device_link_security")
set(HOST_CONNECTIVITY "${PROJECT_ROOT}/tests/connectivity/host")

add_executable(test_device_link_security
    test_device_link_security.c
    "${HOST_CONNECTIVITY}/freertos_host.c"
    "${COMPONENT}/src/device_link_security.c"
)
target_compile_features(test_device_link_security PRIVATE c_std_11)
target_compile_definitions(test_device_link_security PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_include_directories(test_device_link_security PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/fakes"
    "${HOST_CONNECTIVITY}/include"
    "${COMPONENT}/include"
)
target_compile_options(test_device_link_security PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wno-variadic-macros
    -UNDEBUG
)
target_link_libraries(test_device_link_security PRIVATE Threads::Threads)

if(DEVICE_LINK_SECURITY_SANITIZER STREQUAL "address")
    target_compile_options(test_device_link_security PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(test_device_link_security PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
elseif(DEVICE_LINK_SECURITY_SANITIZER STREQUAL "thread")
    target_compile_options(test_device_link_security PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
    target_link_options(test_device_link_security PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
elseif(NOT DEVICE_LINK_SECURITY_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported DEVICE_LINK_SECURITY_SANITIZER=${DEVICE_LINK_SECURITY_SANITIZER}")
endif()

if(DEVICE_LINK_SECURITY_SANITIZER STREQUAL "thread" AND
        DEVICE_LINK_SECURITY_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME device_link_security COMMAND
        ${DEVICE_LINK_SECURITY_SETARCH} x86_64 -R
        $<TARGET_FILE:test_device_link_security>)
else()
    add_test(NAME device_link_security COMMAND test_device_link_security)
endif()
set_tests_properties(device_link_security PROPERTIES TIMEOUT 20)

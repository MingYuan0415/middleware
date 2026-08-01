cmake_minimum_required(VERSION 3.16)
project(power_service_host_regression C)

enable_testing()
find_package(Threads REQUIRED)

set(POWER_SERVICE_SANITIZER "none" CACHE STRING "none, address, or thread")
set_property(CACHE POWER_SERVICE_SANITIZER PROPERTY STRINGS none address thread)
find_program(POWER_SERVICE_SETARCH setarch)

get_filename_component(POWER_SERVICE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(MIDDLEWARE_COMPONENTS_DIR
    "${POWER_SERVICE_DIR}/.." ABSOLUTE)

add_executable(power_service_irq_test
    test_power_service_irq.c
    "${MIDDLEWARE_COMPONENTS_DIR}/time_service/tests/host/freertos_fake.c"
    "${POWER_SERVICE_DIR}/src/power_service.c"
)

target_include_directories(power_service_irq_test PRIVATE
    fakes
    "${MIDDLEWARE_COMPONENTS_DIR}/time_service/tests/host/include"
    "${POWER_SERVICE_DIR}/include"
)

target_compile_features(power_service_irq_test PRIVATE c_std_11)
target_compile_definitions(power_service_irq_test PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_compile_options(power_service_irq_test PRIVATE
    -include "${POWER_SERVICE_DIR}/../../tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
)
target_link_libraries(power_service_irq_test PRIVATE Threads::Threads)

if(POWER_SERVICE_SANITIZER STREQUAL "address")
    target_compile_options(power_service_irq_test PRIVATE
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -fsanitize=address,undefined
    )
    target_link_options(power_service_irq_test PRIVATE
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -fsanitize=address,undefined
    )
elseif(POWER_SERVICE_SANITIZER STREQUAL "thread")
    target_compile_options(power_service_irq_test PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=thread
    )
    target_link_options(power_service_irq_test PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=thread
    )
elseif(NOT POWER_SERVICE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported POWER_SERVICE_SANITIZER=${POWER_SERVICE_SANITIZER}")
endif()

if(POWER_SERVICE_SANITIZER STREQUAL "thread" AND
        POWER_SERVICE_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME power_service_irq_test COMMAND
        ${POWER_SERVICE_SETARCH} x86_64 -R $<TARGET_FILE:power_service_irq_test>)
else()
    add_test(NAME power_service_irq_test COMMAND power_service_irq_test)
endif()
set_tests_properties(power_service_irq_test PROPERTIES TIMEOUT 10)

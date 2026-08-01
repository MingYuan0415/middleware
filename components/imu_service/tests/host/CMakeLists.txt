cmake_minimum_required(VERSION 3.16)
project(imu_service_host_regression C)

enable_testing()
find_package(Threads REQUIRED)

set(IMU_SERVICE_SANITIZER "none" CACHE STRING "none, address, or thread")
set_property(CACHE IMU_SERVICE_SANITIZER PROPERTY STRINGS none address thread)
find_program(IMU_SERVICE_SETARCH setarch)

get_filename_component(IMU_SERVICE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(IMU_SERVICE_HOST_FAKES
    esp_timer_fake.c
    event_bus_fake.c
    freertos_fake.c
    imu_fake.c
)

function(add_imu_service_host_test target source)
    add_executable(${target}
        ${source}
        ${IMU_SERVICE_HOST_FAKES}
        "${IMU_SERVICE_DIR}/src/imu_service.c"
    )
    target_include_directories(${target} PRIVATE
        include
        "${IMU_SERVICE_DIR}/../event_bus/include"
        "${IMU_SERVICE_DIR}/include"
    )
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_definitions(${target} PRIVATE
        _POSIX_C_SOURCE=200809L
    )
    target_compile_options(${target} PRIVATE
        -include "${IMU_SERVICE_DIR}/../../tests/host/sdkconfig.h"
        -Wall
        -Wextra
        -Werror
        -Wpedantic
    )
    target_link_libraries(${target} PRIVATE Threads::Threads)

    if(IMU_SERVICE_SANITIZER STREQUAL "address")
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
    elseif(IMU_SERVICE_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=thread
        )
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=thread
        )
    elseif(NOT IMU_SERVICE_SANITIZER STREQUAL "none")
        message(FATAL_ERROR
            "Unsupported IMU_SERVICE_SANITIZER=${IMU_SERVICE_SANITIZER}")
    endif()

    if(IMU_SERVICE_SANITIZER STREQUAL "thread" AND
            IMU_SERVICE_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        add_test(NAME ${target} COMMAND
            ${IMU_SERVICE_SETARCH} x86_64 -R $<TARGET_FILE:${target}>)
    else()
        add_test(NAME ${target} COMMAND ${target})
    endif()
    set_tests_properties(${target} PROPERTIES TIMEOUT 15)
endfunction()

add_imu_service_host_test(imu_service_lifecycle_test
    test_imu_service_lifecycle.c)
add_imu_service_host_test(imu_service_init_failure_test
    test_imu_service_init_failure.c)
add_imu_service_host_test(imu_service_timeout_test
    test_imu_service_timeout.c)
add_imu_service_host_test(imu_service_concurrency_test
    test_imu_service_concurrency.c)

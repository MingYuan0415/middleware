cmake_minimum_required(VERSION 3.16)
project(mt_device_link_service_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(DEVICE_LINK_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE DEVICE_LINK_SERVICE_SANITIZER PROPERTY STRINGS
             none address thread)
find_program(DEVICE_LINK_SERVICE_SETARCH setarch)

get_filename_component(PROJECT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(COMPONENT
    "${PROJECT_ROOT}/layers/middleware/components/device_link_service")
set(BLE_RUNTIME "${PROJECT_ROOT}/layers/middleware/components/ble_runtime")
set(DEVICE_LINK "${PROJECT_ROOT}/layers/middleware/components/device_link")
set(HOST_CONNECTIVITY "${PROJECT_ROOT}/tests/connectivity/host")

add_executable(test_device_link_wifi_adapter
               test_device_link_wifi_adapter.c
               fakes/connectivity_manager.c
               fakes/event_bus.c
               fakes/ble_link_service_fake.c
               "${COMPONENT}/src/device_link_wifi_adapter.c"
               "${DEVICE_LINK}/src/device_link_v1.c")
target_compile_features(test_device_link_wifi_adapter PRIVATE c_std_11)
target_compile_definitions(test_device_link_wifi_adapter PRIVATE
    _POSIX_C_SOURCE=200809L
    UNIT_TEST_HOST)
target_include_directories(test_device_link_wifi_adapter PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}/fakes"
                           "${COMPONENT}/include"
                           "${BLE_RUNTIME}/include"
                           "${DEVICE_LINK}/include"
                           "${PROJECT_ROOT}/layers/middleware/components/connectivity_manager/include"
                           "${PROJECT_ROOT}/layers/middleware/components/event_bus/include"
                           "${HOST_CONNECTIVITY}/include"
                           "${PROJECT_ROOT}/tests/host")
target_compile_options(test_device_link_wifi_adapter PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG)
add_test(NAME device_link_wifi_adapter COMMAND test_device_link_wifi_adapter)
set_tests_properties(device_link_wifi_adapter PROPERTIES TIMEOUT 10)

add_executable(test_device_link_service
               test_device_link_service.c)
target_compile_features(test_device_link_service PRIVATE c_std_11)
target_compile_definitions(test_device_link_service PRIVATE
    _POSIX_C_SOURCE=200809L
    UNIT_TEST_HOST)
target_include_directories(test_device_link_service PRIVATE
                           "${COMPONENT}/include"
                           "${BLE_RUNTIME}/include"
                           "${PROJECT_ROOT}/layers/middleware/components/event_bus/include"
                           "${HOST_CONNECTIVITY}/include")
target_compile_options(test_device_link_service PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG)
add_test(NAME device_link_service COMMAND test_device_link_service)
set_tests_properties(device_link_service PROPERTIES TIMEOUT 10)

if(DEVICE_LINK_SERVICE_SANITIZER STREQUAL "address")
    foreach(target IN ITEMS test_device_link_wifi_adapter
            test_device_link_service)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
    endforeach()
elseif(DEVICE_LINK_SERVICE_SANITIZER STREQUAL "thread")
    foreach(target IN ITEMS test_device_link_wifi_adapter
            test_device_link_service)
        target_compile_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer)
    endforeach()
elseif(NOT DEVICE_LINK_SERVICE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported DEVICE_LINK_SERVICE_SANITIZER=${DEVICE_LINK_SERVICE_SANITIZER}")
endif()

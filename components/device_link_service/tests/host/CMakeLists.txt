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
               "${HOST_CONNECTIVITY}/freertos_host.c"
               "${COMPONENT}/src/device_link_wifi_adapter.c"
               "${DEVICE_LINK}/src/device_link_v1.c")
target_compile_features(test_device_link_wifi_adapter PRIVATE c_std_11)
target_compile_definitions(test_device_link_wifi_adapter PRIVATE
    _POSIX_C_SOURCE=200809L
    UNIT_TEST_HOST)
target_include_directories(test_device_link_wifi_adapter PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}/fakes"
                           "${COMPONENT}/include"
                           "${COMPONENT}/src"
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
target_link_libraries(test_device_link_wifi_adapter PRIVATE Threads::Threads)
target_link_options(test_device_link_wifi_adapter PRIVATE
    -Wl,--wrap=xSemaphoreTakeRecursive)
add_executable(test_device_link_status_helpers
               test_device_link_status_helpers.c
               "${COMPONENT}/src/device_link_confirmation.c"
               "${COMPONENT}/src/device_link_revoke_progress.c")
target_compile_features(test_device_link_status_helpers PRIVATE c_std_11)
target_compile_definitions(test_device_link_status_helpers PRIVATE
    _POSIX_C_SOURCE=200809L
    UNIT_TEST_HOST
    CONFIG_DEVICE_LINK_SERVICE_QUEUE_DEPTH=8
    CONFIG_DEVICE_LINK_SERVICE_TASK_STACK=6144)
target_include_directories(test_device_link_status_helpers PRIVATE
                           "${COMPONENT}/src"
                           "${COMPONENT}/include"
                           "${COMPONENT}/src"
                           "${BLE_RUNTIME}/include"
                           "${DEVICE_LINK}/include"
                           "${PROJECT_ROOT}/layers/middleware/components/connectivity_manager/include"
                           "${PROJECT_ROOT}/layers/middleware/components/event_bus/include"
                           "${PROJECT_ROOT}/layers/middleware/components/nv_storage/include"
                           "${PROJECT_ROOT}/layers/middleware/components/wifi_service/include"
                           "${HOST_CONNECTIVITY}/include")
target_compile_options(test_device_link_status_helpers PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG)
add_executable(test_device_link_lifecycle
    test_device_link_lifecycle.c
    fakes/lifecycle.c
    "${COMPONENT}/src/device_link_service.c"
    "${COMPONENT}/src/device_link_confirmation.c"
    "${COMPONENT}/src/device_link_revoke_progress.c"
    "${BLE_RUNTIME}/src/ble_runtime.c"
    "${BLE_RUNTIME}/src/ble_event_router.c"
    "${HOST_CONNECTIVITY}/freertos_host.c"
    "${HOST_CONNECTIVITY}/nv_storage_fake.c"
    fakes/event_bus.c)
target_compile_features(test_device_link_lifecycle PRIVATE c_std_11)
target_compile_definitions(test_device_link_lifecycle PRIVATE
    _POSIX_C_SOURCE=200809L UNIT_TEST_HOST
    CONFIG_DEVICE_LINK_SERVICE_QUEUE_DEPTH=8
    CONFIG_DEVICE_LINK_SERVICE_TASK_STACK=6144)
target_compile_options(test_device_link_lifecycle PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
target_include_directories(test_device_link_lifecycle PRIVATE
    fakes "${COMPONENT}/include" "${COMPONENT}/src"
    "${BLE_RUNTIME}/include" "${DEVICE_LINK}/include"
    "${PROJECT_ROOT}/layers/middleware/components/event_bus/include"
    "${PROJECT_ROOT}/layers/middleware/components/nv_storage/include"
    "${HOST_CONNECTIVITY}/include")
target_link_libraries(test_device_link_lifecycle PRIVATE Threads::Threads)
target_link_options(test_device_link_lifecycle PRIVATE
    -Wl,--wrap=nv_storage_set_blob)
add_test(NAME device_link_lifecycle COMMAND test_device_link_lifecycle)
set_tests_properties(device_link_lifecycle PROPERTIES TIMEOUT 20)

if(DEVICE_LINK_SERVICE_SANITIZER STREQUAL "address")
    foreach(target IN ITEMS test_device_link_wifi_adapter
            test_device_link_status_helpers test_device_link_lifecycle)
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
            test_device_link_status_helpers test_device_link_lifecycle)
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

if(DEVICE_LINK_SERVICE_SANITIZER STREQUAL "thread" AND
        DEVICE_LINK_SERVICE_SETARCH AND
        CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME device_link_wifi_adapter COMMAND
        ${DEVICE_LINK_SERVICE_SETARCH} x86_64 -R
        $<TARGET_FILE:test_device_link_wifi_adapter>)
    add_test(NAME device_link_status_helpers COMMAND
        ${DEVICE_LINK_SERVICE_SETARCH} x86_64 -R
        $<TARGET_FILE:test_device_link_status_helpers>)
else()
    add_test(NAME device_link_wifi_adapter COMMAND
        test_device_link_wifi_adapter)
    add_test(NAME device_link_status_helpers COMMAND test_device_link_status_helpers)
endif()
set_tests_properties(device_link_wifi_adapter device_link_status_helpers
    PROPERTIES TIMEOUT 10)

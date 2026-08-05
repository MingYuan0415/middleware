cmake_minimum_required(VERSION 3.16)
project(mt_ble_runtime_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(BLE_RUNTIME_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE BLE_RUNTIME_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(BLE_RUNTIME_SETARCH setarch)

get_filename_component(PROJECT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(COMPONENT "${PROJECT_ROOT}/layers/middleware/components/ble_runtime")
set(ESP_ERR "${PROJECT_ROOT}/tests/connectivity/host/include")

add_executable(test_ble_runtime
    test_ble_runtime.c
    "${COMPONENT}/src/ble_runtime.c"
)
target_compile_features(test_ble_runtime PRIVATE c_std_11)
target_compile_definitions(test_ble_runtime PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_include_directories(test_ble_runtime PRIVATE
    "${COMPONENT}/include"
    "${ESP_ERR}"
)
target_compile_options(test_ble_runtime PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG
)

if(BLE_RUNTIME_SANITIZER STREQUAL "address")
    target_compile_options(test_ble_runtime PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(test_ble_runtime PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
elseif(BLE_RUNTIME_SANITIZER STREQUAL "thread")
    target_compile_options(test_ble_runtime PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
    target_link_options(test_ble_runtime PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
elseif(NOT BLE_RUNTIME_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported BLE_RUNTIME_SANITIZER=${BLE_RUNTIME_SANITIZER}")
endif()

if(BLE_RUNTIME_SANITIZER STREQUAL "thread" AND
        BLE_RUNTIME_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME ble_runtime COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_runtime>)
else()
    add_test(NAME ble_runtime COMMAND test_ble_runtime)
endif()
set_tests_properties(ble_runtime PROPERTIES TIMEOUT 10)

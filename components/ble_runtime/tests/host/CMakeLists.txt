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

foreach(test_target IN ITEMS
        test_ble_runtime
        test_ble_gatt_registry
        test_ble_gap_manager
        test_ble_event_router
        test_ble_adv_manager
        test_ble_tx_scheduler
        test_ble_response_cache
        test_ble_link_codec
        test_ble_link_dispatcher)
    add_executable(${test_target}
        ${test_target}.c
        "${COMPONENT}/src/ble_runtime.c"
        "${COMPONENT}/src/ble_gatt_registry.c"
        "${COMPONENT}/src/ble_gap_manager.c"
        "${COMPONENT}/src/ble_adv_manager.c"
        "${COMPONENT}/src/ble_tx_scheduler.c"
        "${COMPONENT}/src/ble_response_cache.c"
        "${COMPONENT}/src/ble_link_codec.c"
        "${COMPONENT}/src/ble_link_dispatcher.c"
        "${COMPONENT}/src/ble_event_router.c"
    )
    target_compile_features(${test_target} PRIVATE c_std_11)
    target_compile_definitions(${test_target} PRIVATE
        _POSIX_C_SOURCE=200809L
    )
    target_include_directories(${test_target} PRIVATE
        "${COMPONENT}/include"
        "${ESP_ERR}"
    )
    target_compile_options(${test_target} PRIVATE
        -Wall
        -Wextra
        -Werror
        -Wpedantic
        -UNDEBUG
    )
endforeach()

foreach(target test_ble_runtime test_ble_gatt_registry
        test_ble_gap_manager test_ble_event_router test_ble_adv_manager
        test_ble_tx_scheduler test_ble_response_cache test_ble_link_codec
        test_ble_link_dispatcher)
    if(BLE_RUNTIME_SANITIZER STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
    elseif(BLE_RUNTIME_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
    elseif(NOT BLE_RUNTIME_SANITIZER STREQUAL "none")
        message(FATAL_ERROR
            "Unsupported BLE_RUNTIME_SANITIZER=${BLE_RUNTIME_SANITIZER}")
    endif()
endforeach()

if(BLE_RUNTIME_SANITIZER STREQUAL "thread" AND
        BLE_RUNTIME_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME ble_runtime COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_runtime>)
    add_test(NAME ble_gatt_registry COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_gatt_registry>)
    add_test(NAME ble_gap_manager COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_gap_manager>)
    add_test(NAME ble_adv_manager COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_adv_manager>)
    add_test(NAME ble_tx_scheduler COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_tx_scheduler>)
    add_test(NAME ble_response_cache COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_response_cache>)
    add_test(NAME ble_link_codec COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_link_codec>)
    add_test(NAME ble_link_dispatcher COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_link_dispatcher>)
    add_test(NAME ble_event_router COMMAND
        ${BLE_RUNTIME_SETARCH} x86_64 -R
        $<TARGET_FILE:test_ble_event_router>)
else()
    add_test(NAME ble_runtime COMMAND test_ble_runtime)
    add_test(NAME ble_gatt_registry COMMAND test_ble_gatt_registry)
    add_test(NAME ble_gap_manager COMMAND test_ble_gap_manager)
    add_test(NAME ble_adv_manager COMMAND test_ble_adv_manager)
    add_test(NAME ble_tx_scheduler COMMAND test_ble_tx_scheduler)
    add_test(NAME ble_response_cache COMMAND test_ble_response_cache)
    add_test(NAME ble_link_codec COMMAND test_ble_link_codec)
    add_test(NAME ble_link_dispatcher COMMAND test_ble_link_dispatcher)
    add_test(NAME ble_event_router COMMAND test_ble_event_router)
endif()
set_tests_properties(ble_runtime PROPERTIES TIMEOUT 10)
set_tests_properties(ble_gatt_registry PROPERTIES TIMEOUT 10)
set_tests_properties(ble_gap_manager PROPERTIES TIMEOUT 10)
set_tests_properties(ble_adv_manager PROPERTIES TIMEOUT 10)
set_tests_properties(ble_tx_scheduler PROPERTIES TIMEOUT 10)
set_tests_properties(ble_response_cache PROPERTIES TIMEOUT 10)
set_tests_properties(ble_link_codec PROPERTIES TIMEOUT 10)
set_tests_properties(ble_link_dispatcher PROPERTIES TIMEOUT 10)
set_tests_properties(ble_event_router PROPERTIES TIMEOUT 10)

add_test(NAME ble_runtime_idf_assumptions COMMAND
    sh "${COMPONENT}/scripts/check_idf_assumptions.sh")
set_tests_properties(ble_runtime_idf_assumptions PROPERTIES TIMEOUT 10)

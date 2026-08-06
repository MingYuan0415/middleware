cmake_minimum_required(VERSION 3.16)

set(BLE_RUNTIME_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE BLE_RUNTIME_SANITIZER PROPERTY STRINGS
    none address thread)

find_program(BLE_RUNTIME_SETARCH setarch)

get_filename_component(PROJECT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(COMPONENT "${PROJECT_ROOT}/layers/middleware/components/ble_runtime")
set(ESP_ERR "${PROJECT_ROOT}/tests/connectivity/host/include")

project(ble_runtime_host_tests C)

enable_testing()

set(TEST_SOURCES
    "${COMPONENT}/src/ble_runtime.c"
    "${COMPONENT}/src/ble_gatt_registry.c"
    "${COMPONENT}/src/ble_gap_manager.c"
    "${COMPONENT}/src/ble_adv_manager.c"
    "${COMPONENT}/src/ble_tx_scheduler.c"
    "${COMPONENT}/src/ble_response_cache.c"
    "${COMPONENT}/src/ble_link_codec.c"
    "${COMPONENT}/src/ble_link_dispatcher.c"
    "${COMPONENT}/src/ble_link_state.c"
    "${COMPONENT}/src/ble_link_reassembler.c"
    "${COMPONENT}/src/ble_link_session.c"
    "${COMPONENT}/src/ble_link_events.c"
    "${COMPONENT}/src/ble_link_service.c"
    "${COMPONENT}/src/ble_event_router.c")

foreach(test_target IN ITEMS
        test_ble_runtime
        test_ble_gatt_registry
        test_ble_gap_manager
        test_ble_event_router
        test_ble_adv_manager
        test_ble_tx_scheduler
        test_ble_response_cache
        test_ble_link_codec
        test_ble_link_dispatcher
        test_ble_link_state
        test_ble_link_reassembler
        test_ble_link_session
        test_ble_link_events
        test_ble_link_service)
    add_executable(${test_target} ${test_target}.c ${TEST_SOURCES})
    target_compile_features(${test_target} PRIVATE c_std_11)
    target_compile_definitions(${test_target} PRIVATE
        _POSIX_C_SOURCE=200809L
        UNIT_TEST_HOST)
    target_include_directories(${test_target} PRIVATE
        "${COMPONENT}/include"
        "${ESP_ERR}")
    target_compile_options(${test_target} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
    if(BLE_RUNTIME_SANITIZER STREQUAL "address")
        target_compile_options(${test_target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        target_link_options(${test_target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer)
    elseif(BLE_RUNTIME_SANITIZER STREQUAL "thread")
        target_compile_options(${test_target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer)
        target_link_options(${test_target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer)
    elseif(NOT BLE_RUNTIME_SANITIZER STREQUAL "none")
        message(FATAL_ERROR
            "Unsupported BLE_RUNTIME_SANITIZER=${BLE_RUNTIME_SANITIZER}")
    endif()
endforeach()

set(BLE_RUNTIME_TESTS
    ble_runtime ble_gatt_registry ble_gap_manager ble_adv_manager
    ble_tx_scheduler ble_response_cache ble_link_codec
    ble_link_dispatcher ble_link_state ble_link_reassembler
    ble_link_session ble_link_events ble_link_service
    ble_event_router)

if(BLE_RUNTIME_SANITIZER STREQUAL "thread" AND BLE_RUNTIME_SETARCH
        AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    foreach(test_name IN LISTS BLE_RUNTIME_TESTS)
        add_test(NAME ${test_name} COMMAND
            ${BLE_RUNTIME_SETARCH} x86_64 -R $<TARGET_FILE:test_${test_name}>)
    endforeach()
else()
    foreach(test_name IN LISTS BLE_RUNTIME_TESTS)
        add_test(NAME ${test_name} COMMAND $<TARGET_FILE:test_${test_name}>)
    endforeach()
endif()

foreach(test_name IN LISTS BLE_RUNTIME_TESTS)
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 10)
endforeach()

add_test(NAME ble_runtime_idf_assumptions COMMAND
    sh "${COMPONENT}/scripts/check_idf_assumptions.sh")
set_tests_properties(ble_runtime_idf_assumptions PROPERTIES TIMEOUT 10)

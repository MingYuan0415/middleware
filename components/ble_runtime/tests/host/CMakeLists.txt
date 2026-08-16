cmake_minimum_required(VERSION 3.16)

set(BLE_RUNTIME_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE BLE_RUNTIME_SANITIZER PROPERTY STRINGS
    none address thread)

find_program(BLE_RUNTIME_SETARCH setarch)

get_filename_component(PROJECT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(COMPONENT "${PROJECT_ROOT}/layers/middleware/components/ble_runtime")
set(DEVICE_LINK_SECURITY "${PROJECT_ROOT}/layers/middleware/components/device_link_security")
set(DEVICE_LINK_WIRE "${PROJECT_ROOT}/layers/middleware/components/device_link")
set(ESP_ERR "${PROJECT_ROOT}/tests/connectivity/host/include")
set(HOST_CONNECTIVITY "${PROJECT_ROOT}/tests/connectivity/host")
set(PROTOCOMM_PROTO "$ENV{IDF_PATH}/components/protocomm/proto-c")
set(PROTOBUF_C "$ENV{IDF_PATH}/components/protobuf-c/protobuf-c")

project(ble_runtime_host_tests C)

enable_testing()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Contract fixture vectors: generated from the pinned contract checkout
# (contracts/provisioning/fixtures/core/v2). The generated include is a
# build artifact; the fixture checkout is the source of truth.
set(CONTRACT_FIXTURE_INC "${CMAKE_CURRENT_BINARY_DIR}/contract_fixtures.inc")
add_custom_command(
    OUTPUT "${CONTRACT_FIXTURE_INC}"
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_LIST_DIR}/contract_fixtures_generate.py"
        --fixtures "${PROJECT_ROOT}/contracts/provisioning/fixtures/core/v2"
        --out "${CONTRACT_FIXTURE_INC}"
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/contract_fixtures_generate.py"
        "${PROJECT_ROOT}/contracts/provisioning/fixtures/core/v2/wire.json"
        "${PROJECT_ROOT}/contracts/provisioning/fixtures/core/v2/framing.json"
        "${PROJECT_ROOT}/contracts/provisioning/fixtures/core/v2/authorization.json"
        "${PROJECT_ROOT}/contracts/provisioning/fixtures/core/v2/error_responses.json"
        "${PROJECT_ROOT}/contracts/provisioning/fixtures/core/v2/golden.json"
        "${PROJECT_ROOT}/contracts/provisioning/fixtures/domains/wifi/v1/operation_results.json"
    VERBATIM)
add_custom_target(contract_fixtures_inc DEPENDS "${CONTRACT_FIXTURE_INC}")

set(TEST_SOURCES
    "${COMPONENT}/src/ble_runtime.c"
    "${COMPONENT}/src/ble_gatt_registry.c"
    "${COMPONENT}/src/ble_gap_manager.c"
    "${COMPONENT}/src/ble_adv_manager.c"
    "${COMPONENT}/src/ble_nimble_adv_start.c"
    "${COMPONENT}/src/ble_nimble_pairing_gate.c"
    "${COMPONENT}/src/ble_nimble_store_guard.c"
    "${COMPONENT}/src/ble_tx_scheduler.c"
    "${COMPONENT}/src/ble_response_cache.c"
    "${COMPONENT}/src/ble_link_codec.c"
    "${COMPONENT}/src/ble_link_dispatcher.c"
    "${COMPONENT}/src/ble_link_state.c"
    "${COMPONENT}/src/ble_link_reassembler.c"
    "${COMPONENT}/src/ble_link_cleanup_obligation.c"
    "${COMPONENT}/src/ble_link_timer_deadline.c"
    "${COMPONENT}/src/ble_nimble_tx_tracker.c"
    "${COMPONENT}/src/ble_link_session.c"
    "${COMPONENT}/src/ble_link_events.c"
    "${COMPONENT}/src/ble_link_sec_state.c"
    "${COMPONENT}/src/ble_link_service.c"
    "${COMPONENT}/src/ble_link_gatt.c"
    "${COMPONENT}/src/ble_event_router.c"
    "${DEVICE_LINK_WIRE}/src/device_link_core.c"
    "${DEVICE_LINK_WIRE}/src/device_link_digest.c"
    "${DEVICE_LINK_WIRE}/src/device_link_operation.c"
    "${DEVICE_LINK_WIRE}/src/device_link_router.c"
    "${DEVICE_LINK_WIRE}/src/device_link_tlv.c"
    "${DEVICE_LINK_WIRE}/src/device_link_wire.c"
    "$ENV{IDF_PATH}/components/bt/common/tinycrypt/src/sha256.c"
    "$ENV{IDF_PATH}/components/bt/common/tinycrypt/src/utils.c"
    "${DEVICE_LINK_SECURITY}/src/device_link_security.c"
    "${HOST_CONNECTIVITY}/freertos_host.c"
    "${CMAKE_CURRENT_LIST_DIR}/fakes/esp_random.c"
    "${CMAKE_CURRENT_LIST_DIR}/fakes/esp_srp.c"
    "${CMAKE_CURRENT_LIST_DIR}/fakes/nv_storage.c"
    "${CMAKE_CURRENT_LIST_DIR}/fakes/protocomm_security2.c"
    "${PROTOCOMM_PROTO}/constants.pb-c.c"
    "${PROTOCOMM_PROTO}/sec0.pb-c.c"
    "${PROTOCOMM_PROTO}/sec1.pb-c.c"
    "${PROTOCOMM_PROTO}/sec2.pb-c.c"
    "${PROTOCOMM_PROTO}/session.pb-c.c"
    "${PROTOBUF_C}/protobuf-c/protobuf-c.c")

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
        test_ble_link_cleanup_obligation
        test_ble_link_timer_deadline
        test_ble_nimble_peer_identity
        test_ble_nimble_pairing_gate
        test_ble_nimble_store_guard
        test_ble_nimble_store_restore_audit
        test_ble_nimble_store_reset
        test_ble_nimble_tx_tracker
        test_ble_link_session
        test_ble_link_events
        test_ble_link_sec_state
        test_ble_link_service
        test_ble_link_gatt
        test_contract_fixtures)
    if(test_target STREQUAL test_ble_link_timer_deadline)
        add_executable(${test_target}
            ${test_target}.c
            "${COMPONENT}/src/ble_link_timer_deadline.c")
    elseif(test_target STREQUAL test_ble_link_cleanup_obligation)
        add_executable(${test_target}
            ${test_target}.c
            "${COMPONENT}/src/ble_link_cleanup_obligation.c")
    elseif(test_target STREQUAL test_ble_nimble_pairing_gate)
        add_executable(${test_target}
            ${test_target}.c
            "${COMPONENT}/src/ble_nimble_pairing_gate.c")
    elseif(test_target STREQUAL test_ble_nimble_peer_identity)
        add_executable(${test_target}
            ${test_target}.c
            "${COMPONENT}/src/ble_nimble_peer_identity.c")
    elseif(test_target STREQUAL test_ble_nimble_store_guard)
        add_executable(${test_target}
            ${test_target}.c
            "${COMPONENT}/src/ble_nimble_store_guard.c")
    elseif(test_target STREQUAL test_ble_nimble_store_restore_audit)
        add_executable(${test_target}
            ${test_target}.c
            "${COMPONENT}/src/ble_nimble_store_restore_audit.c")
    elseif(test_target STREQUAL test_ble_nimble_store_reset)
        add_executable(${test_target}
            ${test_target}.c
            "${COMPONENT}/src/ble_nimble_store_reset.c")
    elseif(test_target STREQUAL test_ble_nimble_tx_tracker)
        add_executable(${test_target}
            ${test_target}.c
            "${COMPONENT}/src/ble_nimble_tx_tracker.c")
    else()
        add_executable(${test_target} ${test_target}.c ${TEST_SOURCES})
    endif()
    target_compile_features(${test_target} PRIVATE c_std_11)
    target_compile_definitions(${test_target} PRIVATE
        _POSIX_C_SOURCE=200809L
        UNIT_TEST_HOST)
    target_compile_options(${test_target} PRIVATE
        -include "${PROJECT_ROOT}/tests/host/sdkconfig.h")
    target_include_directories(${test_target} PRIVATE
        "${COMPONENT}/include"
        "${COMPONENT}/src"
        "${DEVICE_LINK_SECURITY}/include"
        "${DEVICE_LINK_WIRE}/include"
        "$ENV{IDF_PATH}/components/bt/common/tinycrypt/include"
        "${PROTOCOMM_PROTO}"
        "${PROTOBUF_C}"
        "${CMAKE_CURRENT_LIST_DIR}/fakes"
        "${ESP_ERR}")
    if(test_target STREQUAL test_contract_fixtures)
        target_include_directories(${test_target} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}")
        add_dependencies(${test_target} contract_fixtures_inc)
    endif()
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
    ble_link_cleanup_obligation
    ble_link_timer_deadline
    ble_nimble_peer_identity
    ble_nimble_pairing_gate
    ble_nimble_store_guard
    ble_nimble_store_restore_audit
    ble_nimble_store_reset
    ble_nimble_tx_tracker
    ble_link_session ble_link_events ble_link_sec_state
    ble_link_service ble_link_gatt ble_event_router
    contract_fixtures)

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

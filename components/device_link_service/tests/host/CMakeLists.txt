cmake_minimum_required(VERSION 3.16)
project(mt_device_link_service_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(DEVICE_LINK_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE DEVICE_LINK_SERVICE_SANITIZER PROPERTY STRINGS
             none address thread)
find_program(DEVICE_LINK_SERVICE_SETARCH setarch)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(COMPONENT "${PROJECT_ROOT}/layers/middleware/components/device_link_service")
set(BLE_RUNTIME "${PROJECT_ROOT}/layers/middleware/components/ble_runtime")
set(DEVICE_LINK_SECURITY "${PROJECT_ROOT}/layers/middleware/components/device_link_security")
set(DEVICE_LINK_WIRE "${PROJECT_ROOT}/layers/middleware/components/device_link")
set(EVENT_BUS "${PROJECT_ROOT}/layers/middleware/components/event_bus")
set(WIFI_SERVICE "${PROJECT_ROOT}/layers/middleware/components/wifi_service")
set(HOST_CONNECTIVITY "${PROJECT_ROOT}/tests/connectivity/host")
set(IDF_BT "$ENV{IDF_PATH}/components/bt")
set(PROTOCOMM_PROTO "$ENV{IDF_PATH}/components/protocomm/proto-c")
set(PROTOBUF_C "$ENV{IDF_PATH}/components/protobuf-c/protobuf-c")

add_executable(test_device_link_service
               test_device_link_service.c
               "${HOST_CONNECTIVITY}/freertos_host.c"
               "${COMPONENT}/src/device_link_service.c"
               "${COMPONENT}/src/device_link_wifi_adapter.c"
               "${WIFI_SERVICE}/src/wifi_service_credentials.c"
               "${CMAKE_CURRENT_LIST_DIR}/fakes/connectivity_manager.c"
               "${BLE_RUNTIME}/src/ble_runtime.c"
               "${BLE_RUNTIME}/src/ble_adv_manager.c"
               "${BLE_RUNTIME}/src/ble_link_session.c"
               "${BLE_RUNTIME}/src/ble_event_router.c"
               "${BLE_RUNTIME}/src/ble_link_service.c"
               "${BLE_RUNTIME}/src/ble_link_gatt.c"
               "${BLE_RUNTIME}/src/ble_tx_scheduler.c"
               "${BLE_RUNTIME}/src/ble_link_codec.c"
               "${BLE_RUNTIME}/src/ble_link_dispatcher.c"
               "${BLE_RUNTIME}/src/ble_link_state.c"
               "${BLE_RUNTIME}/src/ble_link_reassembler.c"
               "${BLE_RUNTIME}/src/ble_link_events.c"
               "${BLE_RUNTIME}/src/ble_gatt_registry.c"
               "${DEVICE_LINK_WIRE}/src/device_link_core.c"
               "${DEVICE_LINK_WIRE}/src/device_link_digest.c"
               "${DEVICE_LINK_WIRE}/src/device_link_operation.c"
               "${DEVICE_LINK_WIRE}/src/device_link_router.c"
               "${DEVICE_LINK_WIRE}/src/device_link_tlv.c"
               "${DEVICE_LINK_WIRE}/src/device_link_wire.c"
               "${IDF_BT}/common/tinycrypt/src/sha256.c"
               "${IDF_BT}/common/tinycrypt/src/utils.c"
               "${DEVICE_LINK_SECURITY}/src/device_link_security.c"
               "${CMAKE_CURRENT_LIST_DIR}/fakes/esp_srp.c"
               "${CMAKE_CURRENT_LIST_DIR}/fakes/nv_storage.c"
               "${CMAKE_CURRENT_LIST_DIR}/fakes/protocomm_security2.c"
               "${PROTOCOMM_PROTO}/constants.pb-c.c"
               "${PROTOCOMM_PROTO}/sec0.pb-c.c"
               "${PROTOCOMM_PROTO}/sec1.pb-c.c"
               "${PROTOCOMM_PROTO}/sec2.pb-c.c"
               "${PROTOCOMM_PROTO}/session.pb-c.c"
               "${PROTOBUF_C}/protobuf-c/protobuf-c.c"
              )
target_compile_features(test_device_link_service PRIVATE c_std_11)
target_compile_definitions(test_device_link_service PRIVATE
    _POSIX_C_SOURCE=200809L
    UNIT_TEST_HOST
    CONFIG_DEVICE_LINK_SERVICE_TASK_STACK=6144
    CONFIG_DEVICE_LINK_SERVICE_QUEUE_DEPTH=8
    CONFIG_DEVICE_LINK_SERVICE_WIFI_ADVERTISED=1
)
target_include_directories(test_device_link_service PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}/fakes"
                           "${BLE_RUNTIME}/tests/host/fakes"
                           "${PROTOCOMM_PROTO}"
                           "${PROTOBUF_C}"
                           "${DEVICE_LINK_SECURITY}/include"
                           "${HOST_CONNECTIVITY}/include"
                           "${COMPONENT}/include"
                           "${BLE_RUNTIME}/include"
                           "${DEVICE_LINK_WIRE}/include"
                           "${EVENT_BUS}/include"
                           "${WIFI_SERVICE}/include"
                           "${PROJECT_ROOT}/layers/middleware/components/connectivity_manager/include"
                           "${IDF_BT}/common/tinycrypt/include"
                          )
target_compile_options(test_device_link_service PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wno-variadic-macros
    -UNDEBUG
                      )
target_link_libraries(test_device_link_service PRIVATE Threads::Threads)

if (DEVICE_LINK_SERVICE_SANITIZER STREQUAL "address")
    target_compile_options(test_device_link_service PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(test_device_link_service PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
elseif(DEVICE_LINK_SERVICE_SANITIZER STREQUAL "thread")
    target_compile_options(test_device_link_service PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
    target_link_options(test_device_link_service PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
elseif(NOT DEVICE_LINK_SERVICE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported DEVICE_LINK_SERVICE_SANITIZER=${DEVICE_LINK_SERVICE_SANITIZER}")
endif()

if(DEVICE_LINK_SERVICE_SANITIZER STREQUAL "thread" AND
        DEVICE_LINK_SERVICE_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME device_link_service COMMAND
        ${DEVICE_LINK_SERVICE_SETARCH} x86_64 -R
        $<TARGET_FILE:test_device_link_service>)
else()
add_test(NAME device_link_service COMMAND test_device_link_service)
            endif()
set_tests_properties(device_link_service PROPERTIES TIMEOUT 20)

add_executable(test_device_link_wifi_adapter
               test_device_link_wifi_adapter.c
               fakes/connectivity_manager.c
               fakes/event_bus.c
               fakes/ble_link_service_fake.c
               "${COMPONENT}/src/device_link_wifi_adapter.c"
               "${WIFI_SERVICE}/src/wifi_service_credentials.c"
               "${DEVICE_LINK_WIRE}/src/device_link_tlv.c")
target_compile_features(test_device_link_wifi_adapter PRIVATE c_std_11)
target_compile_definitions(test_device_link_wifi_adapter PRIVATE
    _POSIX_C_SOURCE=200809L
    UNIT_TEST_HOST)
target_include_directories(test_device_link_wifi_adapter PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}/fakes"
                           "${BLE_RUNTIME}/tests/host/fakes"
                           "${COMPONENT}/include"
                           "${BLE_RUNTIME}/include"
                           "${DEVICE_LINK_SECURITY}/include"
                           "${DEVICE_LINK_WIRE}/include"
                           "${PROJECT_ROOT}/layers/middleware/components/connectivity_manager/include"
                           "${PROJECT_ROOT}/layers/middleware/components/event_bus/include"
                           "${WIFI_SERVICE}/include"
                           "${HOST_CONNECTIVITY}/include"
                           "${PROJECT_ROOT}/tests/host"
                          )
target_compile_options(test_device_link_wifi_adapter PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG)
add_test(NAME device_link_wifi_adapter COMMAND test_device_link_wifi_adapter)
set_tests_properties(device_link_wifi_adapter PROPERTIES TIMEOUT 10)

# Contract fixture vectors for the Wi-Fi domain (wifi.v1 invalid.json and
# operation_results.json): generated from the pinned contract checkout and
# consumed by test_contract_fixtures_wifi.c.
set(CONTRACT_WIFI_FIXTURE_INC
    "${CMAKE_CURRENT_BINARY_DIR}/contract_fixtures_wifi.inc")
add_custom_command(
    OUTPUT "${CONTRACT_WIFI_FIXTURE_INC}"
    COMMAND ${Python3_EXECUTABLE}
        "${PROJECT_ROOT}/layers/middleware/components/ble_runtime/tests/host/contract_fixtures_generate.py"
        --fixtures "${PROJECT_ROOT}/contracts/provisioning/fixtures/core/v2"
        --kind wifi
        --out "${CONTRACT_WIFI_FIXTURE_INC}"
    DEPENDS
        "${PROJECT_ROOT}/layers/middleware/components/ble_runtime/tests/host/contract_fixtures_generate.py"
        "${PROJECT_ROOT}/contracts/provisioning/fixtures/domains/wifi/v1/invalid.json"
        "${PROJECT_ROOT}/contracts/provisioning/fixtures/domains/wifi/v1/golden.json"
        "${PROJECT_ROOT}/contracts/provisioning/fixtures/domains/wifi/v1/operation_results.json"
    VERBATIM)
add_custom_target(contract_wifi_fixtures_inc
                  DEPENDS "${CONTRACT_WIFI_FIXTURE_INC}")

add_executable(test_contract_fixtures_wifi
               test_contract_fixtures_wifi.c
               fakes/connectivity_manager.c
               fakes/event_bus.c
               fakes/ble_link_service_fake.c
               "${COMPONENT}/src/device_link_wifi_adapter.c"
               "${WIFI_SERVICE}/src/wifi_service_credentials.c"
               "${DEVICE_LINK_WIRE}/src/device_link_tlv.c")
target_compile_features(test_contract_fixtures_wifi PRIVATE c_std_11)
target_compile_definitions(test_contract_fixtures_wifi PRIVATE
    _POSIX_C_SOURCE=200809L
    UNIT_TEST_HOST)
target_include_directories(test_contract_fixtures_wifi PRIVATE
                           "${CMAKE_CURRENT_BINARY_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/fakes"
                           "${BLE_RUNTIME}/tests/host/fakes"
                           "${COMPONENT}/include"
                           "${BLE_RUNTIME}/include"
                           "${DEVICE_LINK_SECURITY}/include"
                           "${DEVICE_LINK_WIRE}/include"
                           "${PROJECT_ROOT}/layers/middleware/components/connectivity_manager/include"
                           "${PROJECT_ROOT}/layers/middleware/components/event_bus/include"
                           "${WIFI_SERVICE}/include"
                           "${HOST_CONNECTIVITY}/include"
                           "${PROJECT_ROOT}/tests/host"
                          )
target_compile_options(test_contract_fixtures_wifi PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG)
add_dependencies(test_contract_fixtures_wifi contract_wifi_fixtures_inc)
add_test(NAME contract_fixtures_wifi COMMAND test_contract_fixtures_wifi)
set_tests_properties(contract_fixtures_wifi PROPERTIES TIMEOUT 10)

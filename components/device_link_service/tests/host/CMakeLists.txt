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
set(EVENT_BUS "${PROJECT_ROOT}/layers/middleware/components/event_bus")
set(HOST_CONNECTIVITY "${PROJECT_ROOT}/tests/connectivity/host")
set(PROTOCOMM_PROTO "$ENV{IDF_PATH}/components/protocomm/proto-c")
set(PROTOBUF_C "$ENV{IDF_PATH}/components/protobuf-c/protobuf-c")

add_executable(test_device_link_service
               test_device_link_service.c
               "${HOST_CONNECTIVITY}/freertos_host.c"
               "${COMPONENT}/src/device_link_service.c"
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
                           "${EVENT_BUS}/include"
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

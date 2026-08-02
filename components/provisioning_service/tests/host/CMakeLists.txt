cmake_minimum_required(VERSION 3.16)
project(mt_provisioning_protocol_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(PROVISIONING_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE PROVISIONING_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(PROVISIONING_SETARCH setarch)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(COMPONENT "${PROJECT_ROOT}/layers/middleware/components/provisioning_service")
set(CONNECTIVITY "${PROJECT_ROOT}/layers/middleware/components/connectivity_manager")
set(EVENT_BUS "${PROJECT_ROOT}/layers/middleware/components/event_bus")
set(PROTOBUF_C "$ENV{IDF_PATH}/components/protobuf-c/protobuf-c")
set(HOST_CONNECTIVITY "${PROJECT_ROOT}/tests/connectivity/host")

add_executable(test_provisioning_protocol
    test_provisioning_protocol.c
    "${COMPONENT}/src/provisioning_protocol.c"
    "${COMPONENT}/src/generated/microtech/provisioning/v1/types.pb-c.c"
    "${COMPONENT}/src/generated/microtech/provisioning/v1/provisioning.pb-c.c"
    "${PROTOBUF_C}/protobuf-c/protobuf-c.c"
)
target_compile_features(test_provisioning_protocol PRIVATE c_std_11)
target_compile_definitions(test_provisioning_protocol PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_include_directories(test_provisioning_protocol PRIVATE
    "${PROJECT_ROOT}/tests/connectivity/host/include"
    "${COMPONENT}/include"
    "${COMPONENT}/src"
    "${COMPONENT}/src/generated"
    "${CONNECTIVITY}/include"
    "${EVENT_BUS}/include"
    "${PROTOBUF_C}"
)
target_compile_options(test_provisioning_protocol PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wno-variadic-macros
    -UNDEBUG
)

add_executable(test_provisioning_service
    test_provisioning_service.c
    host_provisioning_service.c
    "${HOST_CONNECTIVITY}/freertos_host.c"
    "${COMPONENT}/src/provisioning_service.c"
    "${COMPONENT}/src/provisioning_protocol.c"
    "${COMPONENT}/src/generated/microtech/provisioning/v1/types.pb-c.c"
    "${COMPONENT}/src/generated/microtech/provisioning/v1/provisioning.pb-c.c"
    "${PROTOBUF_C}/protobuf-c/protobuf-c.c"
)
target_compile_features(test_provisioning_service PRIVATE c_std_11)
target_compile_definitions(test_provisioning_service PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_include_directories(test_provisioning_service PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/fakes"
    "${HOST_CONNECTIVITY}/include"
    "${COMPONENT}/include"
    "${COMPONENT}/src"
    "${COMPONENT}/src/generated"
    "${CONNECTIVITY}/include"
    "${EVENT_BUS}/include"
    "${PROTOBUF_C}"
)
target_compile_options(test_provisioning_service PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wno-variadic-macros
    -UNDEBUG
)
target_link_libraries(test_provisioning_service PRIVATE Threads::Threads)

if(PROVISIONING_SANITIZER STREQUAL "address")
    foreach(target test_provisioning_protocol test_provisioning_service)
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
    endforeach()
elseif(PROVISIONING_SANITIZER STREQUAL "thread")
    foreach(target test_provisioning_protocol test_provisioning_service)
        target_compile_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
    endforeach()
elseif(NOT PROVISIONING_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported PROVISIONING_SANITIZER=${PROVISIONING_SANITIZER}")
endif()

if(PROVISIONING_SANITIZER STREQUAL "thread" AND
        PROVISIONING_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME provisioning_protocol COMMAND
        ${PROVISIONING_SETARCH} x86_64 -R
        $<TARGET_FILE:test_provisioning_protocol>)
else()
    add_test(NAME provisioning_protocol COMMAND test_provisioning_protocol)
endif()
set_tests_properties(provisioning_protocol PROPERTIES TIMEOUT 10)
if(PROVISIONING_SANITIZER STREQUAL "thread" AND
        PROVISIONING_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME provisioning_service COMMAND
        ${PROVISIONING_SETARCH} x86_64 -R
        $<TARGET_FILE:test_provisioning_service>)
else()
    add_test(NAME provisioning_service COMMAND test_provisioning_service)
endif()
set_tests_properties(provisioning_service PROPERTIES TIMEOUT 10)

add_test(NAME provisioning_idf_assumptions COMMAND
    sh "${COMPONENT}/scripts/check_idf_assumptions.sh")
set_tests_properties(provisioning_idf_assumptions PROPERTIES TIMEOUT 10)

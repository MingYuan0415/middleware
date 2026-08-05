cmake_minimum_required(VERSION 3.16)
project(mt_device_link_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(DEVICE_LINK_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE DEVICE_LINK_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(DEVICE_LINK_SETARCH setarch)

get_filename_component(PROJECT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(COMPONENT "${PROJECT_ROOT}/layers/middleware/components/device_link")
set(PROTOBUF_C "$ENV{IDF_PATH}/components/protobuf-c/protobuf-c")

add_executable(test_device_link_framing
    test_device_link_framing.c
    "${COMPONENT}/src/device_link_framing.c"
)
target_compile_features(test_device_link_framing PRIVATE c_std_11)
target_compile_definitions(test_device_link_framing PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_include_directories(test_device_link_framing PRIVATE
    "${COMPONENT}/include"
)
target_compile_options(test_device_link_framing PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG
)

add_executable(test_device_link_protobuf
    test_device_link_protobuf.c
    "${COMPONENT}/src/generated/microtech/link/v1/capabilities.pb-c.c"
    "${COMPONENT}/src/generated/microtech/link/v1/envelope.pb-c.c"
    "${COMPONENT}/src/generated/microtech/link/v1/errors.pb-c.c"
    "${COMPONENT}/src/generated/microtech/link/v1/events.pb-c.c"
    "${COMPONENT}/src/generated/microtech/link/v1/session.pb-c.c"
    "${COMPONENT}/src/generated/microtech/link/v1/transfer.pb-c.c"
    "${PROTOBUF_C}/protobuf-c/protobuf-c.c"
)
target_compile_features(test_device_link_protobuf PRIVATE c_std_11)
target_compile_definitions(test_device_link_protobuf PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_include_directories(test_device_link_protobuf PRIVATE
    "${COMPONENT}/src/generated"
    "${PROTOBUF_C}"
)
target_compile_options(test_device_link_protobuf PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG
)

foreach(target test_device_link_framing test_device_link_protobuf)
    if(DEVICE_LINK_SANITIZER STREQUAL "address")
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
    elseif(DEVICE_LINK_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
    elseif(NOT DEVICE_LINK_SANITIZER STREQUAL "none")
        message(FATAL_ERROR
            "Unsupported DEVICE_LINK_SANITIZER=${DEVICE_LINK_SANITIZER}")
    endif()
endforeach()

if(DEVICE_LINK_SANITIZER STREQUAL "thread" AND
        DEVICE_LINK_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME device_link_framing COMMAND
        ${DEVICE_LINK_SETARCH} x86_64 -R
        $<TARGET_FILE:test_device_link_framing>)
    add_test(NAME device_link_protobuf COMMAND
        ${DEVICE_LINK_SETARCH} x86_64 -R
        $<TARGET_FILE:test_device_link_protobuf>)
else()
    add_test(NAME device_link_framing COMMAND test_device_link_framing)
    add_test(NAME device_link_protobuf COMMAND test_device_link_protobuf)
endif()
set_tests_properties(device_link_framing PROPERTIES TIMEOUT 10)
set_tests_properties(device_link_protobuf PROPERTIES TIMEOUT 10)

add_test(NAME device_link_generated_contract COMMAND
    sh "${COMPONENT}/scripts/check_generated.sh")
set_tests_properties(device_link_generated_contract PROPERTIES TIMEOUT 10)

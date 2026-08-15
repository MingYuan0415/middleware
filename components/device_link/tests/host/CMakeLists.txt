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
set(IDF_BT "$ENV{IDF_PATH}/components/bt")

function(add_device_link_test target source)
    add_executable(${target}
        ${source}
        "${COMPONENT}/src/device_link_operation.c"
        "${COMPONENT}/src/device_link_core.c"
        "${COMPONENT}/src/device_link_digest.c"
        "${COMPONENT}/src/device_link_router.c"
        "${COMPONENT}/src/device_link_tlv.c"
        "${COMPONENT}/src/device_link_wire.c"
        "${IDF_BT}/common/tinycrypt/src/sha256.c"
        "${IDF_BT}/common/tinycrypt/src/utils.c"
    )
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_definitions(${target} PRIVATE
        _POSIX_C_SOURCE=200809L
    )
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/fakes"
        "${COMPONENT}/include"
        "${IDF_BT}/common/tinycrypt/include"
    )
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Werror
        -Wpedantic
        -UNDEBUG
    )
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

    if(DEVICE_LINK_SANITIZER STREQUAL "thread" AND
            DEVICE_LINK_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        add_test(NAME ${target} COMMAND
            ${DEVICE_LINK_SETARCH} x86_64 -R $<TARGET_FILE:${target}>)
    else()
        add_test(NAME ${target} COMMAND ${target})
    endif()
    set_tests_properties(${target} PROPERTIES TIMEOUT 10)
endfunction()

add_device_link_test(test_device_link_wire_tlv test_device_link_wire_tlv.c)
add_device_link_test(test_device_link_router_operation
    test_device_link_router_operation.c)

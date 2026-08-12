cmake_minimum_required(VERSION 3.22)
project(chore_service_host C)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(MT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(CHORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(INTEGRATION_HOST "${MT_ROOT}/tests/integration/host")

set(CHORE_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE CHORE_SERVICE_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(CHORE_SERVICE_SETARCH setarch)

function(configure_chore_host_target target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
    if(CHORE_SERVICE_SANITIZER STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer -fno-sanitize-recover=all
            -fsanitize=address,undefined)
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer -fno-sanitize-recover=all
            -fsanitize=address,undefined)
    elseif(CHORE_SERVICE_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer -fsanitize=thread)
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer -fsanitize=thread)
    elseif(NOT CHORE_SERVICE_SANITIZER STREQUAL "none")
        message(FATAL_ERROR
            "Unsupported CHORE_SERVICE_SANITIZER=${CHORE_SERVICE_SANITIZER}")
    endif()
endfunction()

function(add_chore_host_test name target)
    if(CHORE_SERVICE_SANITIZER STREQUAL "thread" AND
            CHORE_SERVICE_SETARCH AND
            CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        add_test(NAME ${name} COMMAND
            ${CHORE_SERVICE_SETARCH} x86_64 -R $<TARGET_FILE:${target}>)
    else()
        add_test(NAME ${name} COMMAND ${target})
    endif()
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

add_executable(chore_service_test
    chore_service_test.c
    "${INTEGRATION_HOST}/host_freertos.c"
    "${CHORE_DIR}/src/chore_service.c")
target_include_directories(chore_service_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    include
    "${INTEGRATION_HOST}/include"
    "${CHORE_DIR}/include")
target_compile_definitions(chore_service_test PRIVATE
    _DEFAULT_SOURCE
    _POSIX_C_SOURCE=200809L)
target_compile_options(chore_service_test PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.h")
target_link_libraries(chore_service_test PRIVATE Threads::Threads)
configure_chore_host_target(chore_service_test)

enable_testing()
add_chore_host_test(chore_service chore_service_test)

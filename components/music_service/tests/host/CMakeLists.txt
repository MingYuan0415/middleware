cmake_minimum_required(VERSION 3.16)
project(music_service_host_tests LANGUAGES C)

enable_testing()

set(MUSIC_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE MUSIC_SERVICE_SANITIZER PROPERTY STRINGS none address thread)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

function(configure_music_host_target target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Werror
        -Wpedantic
        -UNDEBUG
    )
    if(MUSIC_SERVICE_SANITIZER STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
    elseif(MUSIC_SERVICE_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer)
    elseif(NOT MUSIC_SERVICE_SANITIZER STREQUAL "none")
        message(FATAL_ERROR
            "Unsupported MUSIC_SERVICE_SANITIZER=${MUSIC_SERVICE_SANITIZER}")
    endif()
endfunction()

add_executable(music_login_protocol_test
    music_login_protocol_test.c
    ../../src/music_login_protocol.c
)

target_include_directories(music_login_protocol_test PRIVATE
    ../../src
)

configure_music_host_target(music_login_protocol_test)

add_test(NAME music_login_protocol_test COMMAND music_login_protocol_test)

add_executable(music_snapshot_test
    music_snapshot_test.c
    ../../src/music_snapshot.c
)
target_include_directories(music_snapshot_test PRIVATE
    ../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../../../../sim/stubs/include
)
configure_music_host_target(music_snapshot_test)
add_test(NAME music_snapshot_test COMMAND music_snapshot_test)

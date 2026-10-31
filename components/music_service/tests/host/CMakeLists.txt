cmake_minimum_required(VERSION 3.16)
project(music_service_host_tests LANGUAGES C)

enable_testing()

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

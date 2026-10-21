cmake_minimum_required(VERSION 3.16)
project(audio_service_host_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(AUDIO_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE AUDIO_SERVICE_SANITIZER PROPERTY STRINGS none address thread)

get_filename_component(AUDIO_SERVICE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(audio_service_host_test
    test_audio_service.c
    fakes/audio_service_fakes.c
    "${AUDIO_SERVICE_DIR}/src/audio_service.c"
)
target_include_directories(audio_service_host_test PRIVATE
    fakes/include
    "${AUDIO_SERVICE_DIR}/include"
)
target_compile_features(audio_service_host_test PRIVATE c_std_11)
target_compile_options(audio_service_host_test PRIVATE
    -include "${AUDIO_SERVICE_DIR}/../../tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG
)
target_link_libraries(audio_service_host_test PRIVATE Threads::Threads)

if(AUDIO_SERVICE_SANITIZER STREQUAL "address")
    target_compile_options(audio_service_host_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(audio_service_host_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
elseif(AUDIO_SERVICE_SANITIZER STREQUAL "thread")
    target_compile_options(audio_service_host_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(audio_service_host_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT AUDIO_SERVICE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported AUDIO_SERVICE_SANITIZER=${AUDIO_SERVICE_SANITIZER}")
endif()

add_test(NAME audio_service_host_test COMMAND audio_service_host_test)
set_tests_properties(audio_service_host_test PROPERTIES TIMEOUT 10)

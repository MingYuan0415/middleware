cmake_minimum_required(VERSION 3.16)
project(recorder_service_host C)

enable_testing()
find_package(Threads REQUIRED)

set(RECORDER_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE RECORDER_SERVICE_SANITIZER PROPERTY STRINGS none address thread)

set(RECORDER_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(INTEGRATION_HOST "${CMAKE_CURRENT_LIST_DIR}/../../../../../../tests/integration/host")

add_executable(recorder_service_test
    recorder_service_test.c
    recorder_service_fakes.c
    "${RECORDER_DIR}/src/recorder_service.c")
target_include_directories(recorder_service_test PRIVATE
    include
    "${RECORDER_DIR}/include"
    "${RECORDER_DIR}/../audio_service/include"
    "${RECORDER_DIR}/../sd_storage_service/include"
    "${INTEGRATION_HOST}/include")
target_compile_features(recorder_service_test PRIVATE c_std_11)
target_compile_definitions(recorder_service_test PRIVATE
    _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
target_compile_options(recorder_service_test PRIVATE
    -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
if(RECORDER_SERVICE_SANITIZER STREQUAL "address")
    target_compile_options(recorder_service_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(recorder_service_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
elseif(RECORDER_SERVICE_SANITIZER STREQUAL "thread")
    target_compile_options(recorder_service_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(recorder_service_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT RECORDER_SERVICE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unsupported RECORDER_SERVICE_SANITIZER")
endif()
target_link_libraries(recorder_service_test PRIVATE Threads::Threads)

add_test(NAME recorder_service COMMAND recorder_service_test)
set_tests_properties(recorder_service PROPERTIES TIMEOUT 10)

cmake_minimum_required(VERSION 3.16)
project(sd_storage_service_host_tests LANGUAGES C)

enable_testing()

set(SD_STORAGE_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE SD_STORAGE_SERVICE_SANITIZER PROPERTY STRINGS
    none address thread)

add_executable(sd_storage_service_test
    test_sd_storage_service.c
    ../../src/sd_storage_service.c
)

target_include_directories(sd_storage_service_test PRIVATE
    fakes
    ../../include
)

target_compile_features(sd_storage_service_test PRIVATE c_std_11)
target_compile_options(sd_storage_service_test PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/../../../../tests/host/sdkconfig.h"
    -Wall
    -Wextra
    -Werror
    -UNDEBUG
)

if(SD_STORAGE_SERVICE_SANITIZER STREQUAL "address")
    target_compile_options(sd_storage_service_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(sd_storage_service_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
elseif(SD_STORAGE_SERVICE_SANITIZER STREQUAL "thread")
    target_compile_options(sd_storage_service_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(sd_storage_service_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT SD_STORAGE_SERVICE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported SD_STORAGE_SERVICE_SANITIZER=${SD_STORAGE_SERVICE_SANITIZER}")
endif()

add_test(NAME sd_storage_service_test COMMAND sd_storage_service_test)
set_tests_properties(sd_storage_service_test PROPERTIES TIMEOUT 10)

cmake_minimum_required(VERSION 3.16)
project(sd_storage_service_host_tests LANGUAGES C)

enable_testing()

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
)

add_test(NAME sd_storage_service_test COMMAND sd_storage_service_test)

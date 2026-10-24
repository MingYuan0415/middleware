cmake_minimum_required(VERSION 3.16)
project(file_service_host C)

enable_testing()
find_package(Threads REQUIRED)

set(FILE_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE FILE_SERVICE_SANITIZER PROPERTY STRINGS none address thread)

set(FILE_SERVICE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(INTEGRATION_HOST
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../../tests/integration/host")

add_executable(file_service_test
    file_service_test.c
    "${FILE_SERVICE_DIR}/src/file_service.c"
    "${INTEGRATION_HOST}/host_freertos.c"
    "${INTEGRATION_HOST}/esp_timer_fake.c")
target_include_directories(file_service_test PRIVATE
    fakes
    "${FILE_SERVICE_DIR}/include"
    "${FILE_SERVICE_DIR}/../sd_storage_service/include"
    "${FILE_SERVICE_DIR}/../mt_log/include"
    "${INTEGRATION_HOST}/include")
target_compile_features(file_service_test PRIVATE c_std_11)
target_compile_definitions(file_service_test PRIVATE
    _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L
    CONFIG_MAIN_PROJECT_TASK_CORE_ID=1)
target_compile_options(file_service_test PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/../../../../../../tests/host/sdkconfig.h"
    -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
if(FILE_SERVICE_SANITIZER STREQUAL "address")
    target_compile_options(file_service_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(file_service_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
elseif(FILE_SERVICE_SANITIZER STREQUAL "thread")
    target_compile_options(file_service_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(file_service_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT FILE_SERVICE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported FILE_SERVICE_SANITIZER=${FILE_SERVICE_SANITIZER}")
endif()
target_link_libraries(file_service_test PRIVATE Threads::Threads)
target_link_options(file_service_test PRIVATE
    -Wl,--wrap=xTaskCreatePinnedToCoreWithCaps
    -Wl,--wrap=opendir -Wl,--wrap=readdir -Wl,--wrap=lstat
    -Wl,--wrap=rmdir -Wl,--wrap=unlink -Wl,--wrap=xQueueSend)

add_test(NAME file_service COMMAND file_service_test)
set_tests_properties(file_service PROPERTIES TIMEOUT 60)

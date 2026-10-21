cmake_minimum_required(VERSION 3.16)
project(system_pm_host_tests C)

enable_testing()
find_package(Threads REQUIRED)

set(SYSTEM_PM_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(PROJECT_ROOT "${SYSTEM_PM_DIR}/../../../..")
set(HOST_COMMON "${PROJECT_ROOT}/tests/integration/host")

set(SYSTEM_PM_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE SYSTEM_PM_SANITIZER PROPERTY STRINGS none address thread)
find_program(SYSTEM_PM_SETARCH setarch)

add_executable(system_pm_test
    system_pm_test.c
    fakes/system_pm_fakes.c
    "${HOST_COMMON}/host_freertos.c"
    "${SYSTEM_PM_DIR}/src/system_pm.c")
target_include_directories(system_pm_test PRIVATE
    fakes
    fakes/include
    "${SYSTEM_PM_DIR}/include"
    "${HOST_COMMON}/include")
target_compile_features(system_pm_test PRIVATE c_std_11)
target_compile_definitions(system_pm_test PRIVATE
    _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L
    CONFIG_MAIN_PROJECT_TASK_CORE_ID=0
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0=0
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1=0)
target_compile_options(system_pm_test PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
target_link_libraries(system_pm_test PRIVATE Threads::Threads)

if(SYSTEM_PM_SANITIZER STREQUAL "address")
    target_compile_options(system_pm_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(system_pm_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
elseif(SYSTEM_PM_SANITIZER STREQUAL "thread")
    target_compile_options(system_pm_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(system_pm_test PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT SYSTEM_PM_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported SYSTEM_PM_SANITIZER=${SYSTEM_PM_SANITIZER}")
endif()

if(SYSTEM_PM_SANITIZER STREQUAL "thread" AND SYSTEM_PM_SETARCH AND
        CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME system_pm COMMAND
        ${SYSTEM_PM_SETARCH} x86_64 -R $<TARGET_FILE:system_pm_test>)
else()
    add_test(NAME system_pm COMMAND system_pm_test)
endif()
set_tests_properties(system_pm PROPERTIES TIMEOUT 20)

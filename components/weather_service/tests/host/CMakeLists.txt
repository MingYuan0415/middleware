cmake_minimum_required(VERSION 3.22)
project(weather_service_host C)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(MT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(WEATHER_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(CJSON_DIR "${MT_ROOT}/managed_components/espressif__cjson/cJSON")
set(INTEGRATION_HOST "${MT_ROOT}/tests/integration/host")

set(WEATHER_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE WEATHER_SERVICE_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(WEATHER_SERVICE_SETARCH setarch)

function(configure_weather_host_target target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
    if(WEATHER_SERVICE_SANITIZER STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer -fno-sanitize-recover=all
            -fsanitize=address,undefined)
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer -fno-sanitize-recover=all
            -fsanitize=address,undefined)
    elseif(WEATHER_SERVICE_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer -fsanitize=thread)
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer -fsanitize=thread)
    elseif(NOT WEATHER_SERVICE_SANITIZER STREQUAL "none")
        message(FATAL_ERROR
            "Unsupported WEATHER_SERVICE_SANITIZER=${WEATHER_SERVICE_SANITIZER}")
    endif()
endfunction()

function(add_weather_host_test name target)
    if(WEATHER_SERVICE_SANITIZER STREQUAL "thread" AND
            WEATHER_SERVICE_SETARCH AND
            CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        add_test(NAME ${name} COMMAND
            ${WEATHER_SERVICE_SETARCH} x86_64 -R $<TARGET_FILE:${target}>)
    else()
        add_test(NAME ${name} COMMAND ${target})
    endif()
    set_tests_properties(${name} PROPERTIES TIMEOUT 15)
endfunction()

add_executable(weather_service_parse_cache_test
    weather_service_parse_cache_test.c
    "${WEATHER_DIR}/src/weather_service_parse.c"
    "${WEATHER_DIR}/src/weather_service_cache.c"
    "${CJSON_DIR}/cJSON.c")
target_include_directories(weather_service_parse_cache_test PRIVATE
    include
    "${WEATHER_DIR}/include"
    "${WEATHER_DIR}/src"
    "${CJSON_DIR}")
configure_weather_host_target(weather_service_parse_cache_test)
target_link_libraries(weather_service_parse_cache_test PRIVATE m)

add_executable(weather_service_worker_test
    weather_service_worker_test.c
    weather_service_worker_fakes.c
    "${INTEGRATION_HOST}/host_freertos.c"
    "${WEATHER_DIR}/src/weather_service.c")
target_include_directories(weather_service_worker_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    include
    "${INTEGRATION_HOST}/include"
    "${WEATHER_DIR}/include"
    "${WEATHER_DIR}/src")
target_compile_definitions(weather_service_worker_test PRIVATE
    _DEFAULT_SOURCE
    _POSIX_C_SOURCE=200809L)
target_compile_options(weather_service_worker_test PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.h")
target_link_libraries(weather_service_worker_test PRIVATE Threads::Threads)
configure_weather_host_target(weather_service_worker_test)

enable_testing()
add_weather_host_test(weather_service_parse_cache
    weather_service_parse_cache_test)
add_weather_host_test(weather_service_worker weather_service_worker_test)

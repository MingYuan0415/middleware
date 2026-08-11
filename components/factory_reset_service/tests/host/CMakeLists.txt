cmake_minimum_required(VERSION 3.16)
project(mt_factory_reset_service_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(FACTORY_RESET_SERVICE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE FACTORY_RESET_SERVICE_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(FACTORY_RESET_SERVICE_SETARCH setarch)

get_filename_component(PROJECT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(COMPONENT
    "${PROJECT_ROOT}/layers/middleware/components/factory_reset_service")
set(HOST_CONNECTIVITY "${PROJECT_ROOT}/tests/connectivity/host")

add_executable(test_factory_reset_service
    test_factory_reset_service.c
    "${CMAKE_CURRENT_LIST_DIR}/fakes/nv_storage.c"
    "${COMPONENT}/src/factory_reset_service.c"
)

target_compile_features(test_factory_reset_service PRIVATE c_std_11)
target_compile_definitions(test_factory_reset_service PRIVATE
    _POSIX_C_SOURCE=200809L
    UNIT_TEST_HOST
)
target_include_directories(test_factory_reset_service PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/fakes"
    "${HOST_CONNECTIVITY}/include"
    "${COMPONENT}/include"
)
target_compile_options(test_factory_reset_service PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wno-variadic-macros
    -UNDEBUG
)
target_link_libraries(test_factory_reset_service PRIVATE Threads::Threads)

if(FACTORY_RESET_SERVICE_SANITIZER STREQUAL "address")
    target_compile_options(test_factory_reset_service PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(test_factory_reset_service PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
elseif(FACTORY_RESET_SERVICE_SANITIZER STREQUAL "thread")
    target_compile_options(test_factory_reset_service PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
    target_link_options(test_factory_reset_service PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
elseif(NOT FACTORY_RESET_SERVICE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported FACTORY_RESET_SERVICE_SANITIZER=${FACTORY_RESET_SERVICE_SANITIZER}")
endif()

if(FACTORY_RESET_SERVICE_SANITIZER STREQUAL "thread" AND
        FACTORY_RESET_SERVICE_SETARCH AND
        CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME factory_reset_service COMMAND
        ${FACTORY_RESET_SERVICE_SETARCH} x86_64 -R
        $<TARGET_FILE:test_factory_reset_service>)
else()
    add_test(NAME factory_reset_service COMMAND test_factory_reset_service)
endif()
set_tests_properties(factory_reset_service PROPERTIES TIMEOUT 20)

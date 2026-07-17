cmake_minimum_required(VERSION 3.16)
project(nv_storage_host_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(NV_STORAGE_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE NV_STORAGE_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(NV_STORAGE_SETARCH setarch)

get_filename_component(NV_STORAGE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(nv_storage_host_test
    test_nv_storage.c
    fakes/fake_nvs.c
    fakes/freertos_host.c
    "${NV_STORAGE_DIR}/src/nv_storage.c"
    "${NV_STORAGE_DIR}/src/nv_storage_blob.c"
)
target_include_directories(nv_storage_host_test PRIVATE
    fakes/include
    "${NV_STORAGE_DIR}/include"
    "${NV_STORAGE_DIR}/src"
)
target_compile_features(nv_storage_host_test PRIVATE c_std_11)
target_compile_definitions(nv_storage_host_test PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_compile_options(nv_storage_host_test PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -UNDEBUG
)
target_link_libraries(nv_storage_host_test PRIVATE Threads::Threads)

if(NV_STORAGE_SANITIZER STREQUAL "address")
    target_compile_options(nv_storage_host_test PRIVATE
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -fsanitize=address,undefined
    )
    target_link_options(nv_storage_host_test PRIVATE
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -fsanitize=address,undefined
    )
    set(NV_STORAGE_TEST_ENVIRONMENT
        "ASAN_OPTIONS=allocator_may_return_null=1")
elseif(NV_STORAGE_SANITIZER STREQUAL "thread")
    target_compile_options(nv_storage_host_test PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=thread
    )
    target_link_options(nv_storage_host_test PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=thread
    )
    set(NV_STORAGE_TEST_ENVIRONMENT
        "TSAN_OPTIONS=allocator_may_return_null=1")
elseif(NOT NV_STORAGE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported NV_STORAGE_SANITIZER=${NV_STORAGE_SANITIZER}")
endif()

if(NV_STORAGE_SANITIZER STREQUAL "thread" AND NV_STORAGE_SETARCH AND
        CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME nv_storage_host_test COMMAND
        ${NV_STORAGE_SETARCH} x86_64 -R
        $<TARGET_FILE:nv_storage_host_test>)
else()
    add_test(NAME nv_storage_host_test COMMAND nv_storage_host_test)
endif()
set_tests_properties(nv_storage_host_test PROPERTIES TIMEOUT 15)
if(NV_STORAGE_TEST_ENVIRONMENT)
    set_tests_properties(nv_storage_host_test PROPERTIES
        ENVIRONMENT "${NV_STORAGE_TEST_ENVIRONMENT}")
endif()

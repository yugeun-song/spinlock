cmake_minimum_required(VERSION 3.21)
foreach(policy CMP0137 CMP0138 CMP0174)
    if(POLICY ${policy})
        cmake_policy(SET ${policy} NEW)
    endif()
endforeach()
project(spinlock C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(SUPPORTED_OSES Linux)
set(SUPPORTED_ARCHES x86_64 aarch64)
set(BUILD_CONFIGS Debug Sanitize Release)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(IS_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES ${BUILD_CONFIGS} CACHE STRING "" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        list(GET BUILD_CONFIGS 0 default_config)
        set(CMAKE_BUILD_TYPE ${default_config} CACHE STRING "one of ${BUILD_CONFIGS}" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${BUILD_CONFIGS})
endif()

include(cmake/platform.cmake)
include(cmake/flags.cmake)

set(BIN_DIR "${CMAKE_SOURCE_DIR}/bin/${TARGET_TAG}-$<LOWER_CASE:$<CONFIG>>")
report("programs" "bin/${TARGET_TAG}-<mode>/")

add_compile_options(-Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wcast-qual
                    -Wwrite-strings -Wvla -Wundef -Wnull-dereference -Wimplicit-fallthrough
                    -Wdouble-promotion -Wconversion -Wsign-conversion)
add_compile_definitions($<$<CONFIG:Debug,Sanitize>:SPINLOCK_DEBUG>)
add_compile_options($<$<CONFIG:Sanitize>:-ftls-model=local-exec>)
if(TARGET_ARCH STREQUAL "aarch64")
    option(ARCH_LSE "target the aarch64 v8.1 LSE atomics (casa, swpal, casl)" OFF)
    if(ARCH_LSE)
        add_compile_options(-march=armv8.1-a)
        report("atomics" "-march=armv8.1-a, LSE (ARCH_LSE is ON)")
    else()
        add_compile_options(-march=armv8-a)
        report("atomics" "-march=armv8-a, load and store exclusive (ARCH_LSE is OFF)")
    endif()
endif()
find_package(Threads REQUIRED)

function(add_program name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE Threads::Threads)
    target_compile_options(${name} PRIVATE $<$<CONFIG:Debug,Sanitize>:${PROFILE_FLAGS}>)
    target_link_options(${name} PRIVATE $<$<CONFIG:Debug,Sanitize>:${PROFILE_FLAGS}>)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}")
endfunction()

add_program(spinlock_test spinlock.c spinlock_test.c main.c)
add_program(spinlock_check spinlock.c spinlock_check.c)

enable_testing()
add_test(NAME spinlock_check COMMAND spinlock_check)
add_test(NAME benchmark_smoke
         COMMAND ${CMAKE_COMMAND} "-DBENCH=$<TARGET_FILE:spinlock_test>"
                 "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}" -P ${CMAKE_SOURCE_DIR}/cmake/check_bench.cmake)
set_tests_properties(spinlock_check benchmark_smoke PROPERTIES
                     ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1;UBSAN_OPTIONS=print_stacktrace=1")

flush_report()

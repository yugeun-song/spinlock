cmake_minimum_required(VERSION 3.16)
project(spinlock LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Emit build/compile_commands.json so clangd / lsp pick up the exact CFLAGS,
# include paths, and standard. Suppresses spurious diagnostics that arise when
# the LSP guesses defaults (e.g. compound-literal warnings, missing macros).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

set(SRCS spinlock_test.c main.c)

set(COMMON_OPTS
    -Wall
    -Wextra
    -fno-omit-frame-pointer
    -fasynchronous-unwind-tables
)

# aarch64: ARMv8-A baseline emits the ldaxr/stlxr LL/SC atomics that run on every
# ARMv8 core. Turn on the ARMv8.1-A LSE fast path (casa/swpal/casl) explicitly
# with -DARCH_LSE=ON. x86-64 needs no arch flag. The target is read from the
# compiler's own triple so it is correct for both native and cross builds
# (e.g. -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc); CMAKE_SYSTEM_PROCESSOR would
# reflect the host unless a toolchain file overrides it.
execute_process(COMMAND ${CMAKE_C_COMPILER} -dumpmachine
                OUTPUT_VARIABLE TARGET_TRIPLE OUTPUT_STRIP_TRAILING_WHITESPACE)
if(TARGET_TRIPLE MATCHES "aarch64")
    option(ARCH_LSE "Target ARMv8.1-A LSE atomics (casa/swpal/casl)" OFF)
    if(ARCH_LSE)
        list(APPEND COMMON_OPTS -march=armv8.1-a)
    else()
        list(APPEND COMMON_OPTS -march=armv8-a)
    endif()
endif()

set(TRACE_OPTS
    -O0
    -g3
    -fno-inline
    -fno-inline-functions
    -fno-optimize-sibling-calls
)

find_package(Threads REQUIRED)

add_executable(spinlock_test ${SRCS})
target_compile_options(spinlock_test PRIVATE ${COMMON_OPTS} -O3)
target_link_libraries(spinlock_test PRIVATE Threads::Threads rt)

add_executable(spinlock_test_trace ${SRCS})
target_compile_options(spinlock_test_trace PRIVATE ${COMMON_OPTS} ${TRACE_OPTS})
target_link_options(spinlock_test_trace PRIVATE -rdynamic)
target_link_libraries(spinlock_test_trace PRIVATE Threads::Threads rt)

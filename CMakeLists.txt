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

set(SRCS spinlock_test.c test.c)

set(COMMON_OPTS
    -Wall
    -Wextra
    -fno-omit-frame-pointer
    -fasynchronous-unwind-tables
)

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

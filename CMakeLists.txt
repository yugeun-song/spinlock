cmake_minimum_required(VERSION 3.16)
project(spinlock LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Emit build/compile_commands.json so clangd / lsp pick up the exact CFLAGS,
# include paths, and standard. The 'index' target copies it to the source root
# (where clangd looks first) and refreshes tags / cscope, mirroring 'make all'.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LIB_SRCS spinlock.c)
set(BENCH_SRCS spinlock_test.c main.c)
set(CHECK_SRCS spinlock_check.c)
set(HDRS spinlock.h spinlock_test.h)

set(WARN_OPTS
    -Wall
    -Wextra
    -Wshadow
    -Wformat=2
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wpointer-arith
    -Wcast-qual
    -Wwrite-strings
    -Wvla
    -Wundef
    -Wnull-dereference
    -Wimplicit-fallthrough
    -Wdouble-promotion
    -Wconversion
    -Wsign-conversion
)

set(HARDEN_OPTS
    -fstack-protector-strong
    -fstack-clash-protection
    -fPIE
)
set(HARDEN_LINK_OPTS -pie -Wl,-z,relro,-z,now,-z,noexecstack)

set(COMMON_OPTS
    ${WARN_OPTS}
    ${HARDEN_OPTS}
    -fno-omit-frame-pointer
    -fasynchronous-unwind-tables
)

# aarch64: the v8.0 baseline emits the ldaxr/stlxr LL/SC atomics that run on every
# aarch64 core. Turn on the v8.1 LSE fast path (casa/swpal/casl) explicitly
# with -DARCH_LSE=ON. x86-64 needs no arch flag. The target is read from the
# compiler's own triple so it is correct for both native and cross builds
# (e.g. -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc); CMAKE_SYSTEM_PROCESSOR would
# reflect the host unless a toolchain file overrides it. The control-flow
# hardening flag differs per ISA and is picked here as well.
execute_process(COMMAND ${CMAKE_C_COMPILER} -dumpmachine
                OUTPUT_VARIABLE TARGET_TRIPLE OUTPUT_STRIP_TRAILING_WHITESPACE)
if(TARGET_TRIPLE MATCHES "aarch64")
    option(ARCH_LSE "Target the aarch64 v8.1 LSE atomics (casa/swpal/casl)" OFF)
    if(ARCH_LSE)
        list(APPEND COMMON_OPTS -march=armv8.1-a)
    else()
        list(APPEND COMMON_OPTS -march=armv8-a)
    endif()
    list(APPEND COMMON_OPTS -mbranch-protection=standard)
else()
    list(APPEND COMMON_OPTS -fcf-protection=full)
endif()

string(REGEX MATCH "^[^-]+" TARGET_ARCHITECTURE "${TARGET_TRIPLE}")
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(COMPILER_FAMILY clang)
else()
    set(COMPILER_FAMILY gcc)
endif()
set(TARGET_BIN_DIR ${CMAKE_SOURCE_DIR}/bin/${TARGET_ARCHITECTURE}-linux-${COMPILER_FAMILY})

set(RELEASE_OPTS -O3 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)

set(DEBUG_OPTS
    -O0
    -g3
    -DSPINLOCK_DEBUG
    -fno-inline
    -fno-inline-functions
    -fno-optimize-sibling-calls
)

find_package(Threads REQUIRED)

add_executable(spinlock_test ${LIB_SRCS} ${BENCH_SRCS})
target_compile_options(spinlock_test PRIVATE ${COMMON_OPTS} ${RELEASE_OPTS})
target_link_options(spinlock_test PRIVATE ${HARDEN_LINK_OPTS})
target_link_libraries(spinlock_test PRIVATE Threads::Threads)

add_executable(spinlock_test_debug ${LIB_SRCS} ${BENCH_SRCS})
target_compile_options(spinlock_test_debug PRIVATE ${COMMON_OPTS} ${DEBUG_OPTS})
target_link_options(spinlock_test_debug PRIVATE -rdynamic ${HARDEN_LINK_OPTS})
target_link_libraries(spinlock_test_debug PRIVATE Threads::Threads)

add_executable(spinlock_check ${LIB_SRCS} ${CHECK_SRCS})
target_compile_options(spinlock_check PRIVATE ${COMMON_OPTS} ${RELEASE_OPTS})
target_link_options(spinlock_check PRIVATE ${HARDEN_LINK_OPTS})
target_link_libraries(spinlock_check PRIVATE Threads::Threads)

add_executable(spinlock_check_debug ${LIB_SRCS} ${CHECK_SRCS})
target_compile_options(spinlock_check_debug PRIVATE ${COMMON_OPTS} ${DEBUG_OPTS})
target_link_options(spinlock_check_debug PRIVATE -rdynamic ${HARDEN_LINK_OPTS})
target_link_libraries(spinlock_check_debug PRIVATE Threads::Threads)

set_target_properties(spinlock_test spinlock_check PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY ${TARGET_BIN_DIR}/release)
set_target_properties(spinlock_test_debug spinlock_check_debug PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY ${TARGET_BIN_DIR}/debug)
set_target_properties(spinlock_test_debug PROPERTIES OUTPUT_NAME spinlock_test)
set_target_properties(spinlock_check_debug PROPERTIES OUTPUT_NAME spinlock_check)

enable_testing()
add_test(NAME spinlock_check COMMAND spinlock_check)
add_test(NAME spinlock_check_debug COMMAND spinlock_check_debug)

# Source-navigation databases in the source root, run with 'cmake --build build
# --target index'. Tools that are absent are skipped, never fatal.
find_program(CTAGS_EXECUTABLE ctags)
find_program(CSCOPE_EXECUTABLE cscope)

set(INDEX_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_BINARY_DIR}/compile_commands.json
            ${CMAKE_SOURCE_DIR}/compile_commands.json)
if(CTAGS_EXECUTABLE)
    list(APPEND INDEX_COMMANDS
         COMMAND ${CTAGS_EXECUTABLE} --kinds-C=+px --fields=+iaSn --extras=+q
                 -f ${CMAKE_SOURCE_DIR}/tags ${LIB_SRCS} ${BENCH_SRCS} ${CHECK_SRCS} ${HDRS})
endif()
if(CSCOPE_EXECUTABLE)
    list(APPEND INDEX_COMMANDS
         COMMAND ${CMAKE_COMMAND} -E echo ${LIB_SRCS} ${BENCH_SRCS} ${CHECK_SRCS} ${HDRS}
         COMMAND sh -c "printf '%s\\n' ${LIB_SRCS} ${BENCH_SRCS} ${CHECK_SRCS} ${HDRS} > cscope.files && ${CSCOPE_EXECUTABLE} -b -q -k -i cscope.files")
endif()

add_custom_target(index
    ${INDEX_COMMANDS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Refreshing compile_commands.json, tags and cscope.out in the source root"
    VERBATIM)

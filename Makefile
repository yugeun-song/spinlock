CC ?= gcc
BIN_DIR := bin
SRCS := spinlock_test.c test.c

TARGET_RELEASE := $(BIN_DIR)/spinlock_test
TARGET_TRACE := $(BIN_DIR)/spinlock_test_trace

# Target-architecture flags. Keyed off the compiler's own target triple (via
# -dumpmachine) so it is correct for both native and cross builds, e.g.
#   make CC=aarch64-linux-gnu-gcc
# On aarch64 the ARMv8-A baseline emits the ldaxr/stlxr LL/SC atomics that run on
# every ARMv8 core. Opt into the ARMv8.1-A LSE fast path (casa / swpal / casl,
# which sets __ARM_FEATURE_ATOMICS) explicitly with:
#   make ARCH_CFLAGS='-march=armv8.1-a'
# x86-64 needs no arch flag (the custom asm targets the base ISA).
TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq (,$(findstring aarch64,$(TARGET_TRIPLE)))
ARCH_CFLAGS ?= -march=armv8-a
endif

WARN_FLAGS := -Wall -Wextra
COMMON_CFLAGS := -std=gnu99 $(WARN_FLAGS) $(ARCH_CFLAGS) -fno-omit-frame-pointer -fasynchronous-unwind-tables
LDLIBS := -pthread -lrt

RELEASE_CFLAGS := -O3 $(COMMON_CFLAGS)
TRACE_CFLAGS := -O0 -g3 $(COMMON_CFLAGS) \
                -fno-inline -fno-inline-functions \
                -fno-optimize-sibling-calls
TRACE_LDFLAGS := -rdynamic

.PHONY: all release trace clean distclean

all: release trace

release: $(TARGET_RELEASE)

trace: $(TARGET_TRACE)

$(TARGET_RELEASE): $(SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(RELEASE_CFLAGS) $(SRCS) -o $@ $(LDLIBS)

$(TARGET_TRACE): $(SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TRACE_CFLAGS) $(SRCS) -o $@ $(TRACE_LDFLAGS) $(LDLIBS)

# Build artifacts only.
clean:
	rm -rf $(BIN_DIR) build *.o

# clean + every debugger / profiler / tracer / cache file the workflow can drop.
distclean: clean
	rm -f core core.* gdb.txt peda-session-*.txt
	rm -f vgcore.* callgrind.out.* cachegrind.out.* massif.out.* helgrind.out.* drd.out.*
	rm -f valgrind.log valgrind-*.log *.vgresult
	rm -f strace.out strace.log *.strace ltrace.out ltrace.log *.ltrace
	rm -rf uftrace.data uftrace.data.old
	rm -f perf.data perf.data.old flamegraph.svg gmon.out
	rm -rf __pycache__ .mypy_cache .ruff_cache
	rm -rf CMakeFiles
	rm -f CMakeCache.txt cmake_install.cmake compile_commands.json

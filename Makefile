CC ?= gcc
BIN_DIR := bin
SRCS := spinlock_test.c test.c

TARGET_RELEASE := $(BIN_DIR)/spinlock_test
TARGET_TRACE := $(BIN_DIR)/spinlock_test_trace

WARN_FLAGS := -Wall -Wextra
COMMON_CFLAGS := -std=gnu17 $(WARN_FLAGS) -fno-omit-frame-pointer -fasynchronous-unwind-tables
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

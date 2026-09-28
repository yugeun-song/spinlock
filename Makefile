CC ?= gcc
BIN_DIR := bin
LIB_SRCS := spinlock.c
BENCH_SRCS := spinlock_test.c main.c
CHECK_SRCS := spinlock_check.c
SRCS := $(LIB_SRCS) $(BENCH_SRCS) $(CHECK_SRCS)
HDRS := spinlock.h spinlock_test.h

TARGET_RELEASE := $(BIN_DIR)/spinlock_test
TARGET_TRACE := $(BIN_DIR)/spinlock_test_trace
TARGET_CHECK_RELEASE := $(BIN_DIR)/spinlock_check
TARGET_CHECK_TRACE := $(BIN_DIR)/spinlock_check_trace
TARGET_CHECK_SANITIZE := $(BIN_DIR)/spinlock_check_sanitize

# Target-architecture flags. Keyed off the compiler's own target triple (via
# -dumpmachine) so it is correct for both native and cross builds, e.g.
#   make CC=aarch64-linux-gnu-gcc
# On aarch64 the v8.0 baseline emits the ldaxr/stlxr LL/SC atomics that run on
# every aarch64 core. Opt into the v8.1 LSE fast path (casa / swpal / casl,
# which sets __ARM_FEATURE_ATOMICS) explicitly with:
#   make ARCH_CFLAGS='-march=armv8.1-a'
# x86-64 needs no arch flag (the custom asm targets the base ISA). The
# control-flow hardening flag differs per ISA, so it is chosen here too.
TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq (,$(findstring aarch64,$(TARGET_TRIPLE)))
ARCH_CFLAGS ?= -march=armv8-a
CFI_CFLAGS := -mbranch-protection=standard
else
CFI_CFLAGS := -fcf-protection=full
endif

WARN_FLAGS := -Wall -Wextra -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes \
              -Wpointer-arith -Wcast-qual -Wwrite-strings -Wvla -Wundef \
              -Wnull-dereference -Wimplicit-fallthrough -Wdouble-promotion \
              -Wconversion -Wsign-conversion
HARDEN_CFLAGS := -fstack-protector-strong -fstack-clash-protection $(CFI_CFLAGS) -fPIE
HARDEN_LDFLAGS := -pie -Wl,-z,relro,-z,now,-z,noexecstack
COMMON_CFLAGS := -std=gnu99 $(WARN_FLAGS) $(ARCH_CFLAGS) $(HARDEN_CFLAGS) \
                 -fno-omit-frame-pointer -fasynchronous-unwind-tables
LDLIBS := -pthread

RELEASE_CFLAGS := -O3 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 $(COMMON_CFLAGS)
TRACE_CFLAGS := -O0 -g3 -DSPINLOCK_DEBUG $(COMMON_CFLAGS) \
                -fno-inline -fno-inline-functions \
                -fno-optimize-sibling-calls
TRACE_LDFLAGS := -rdynamic
# -ftls-model=local-exec is valid here (every thread-local lives in this
# executable) and sidesteps a GCC 16 -fsanitize=null false positive: with the
# initial-exec or dynamic TLS models its inlined check reports the address of
# the extern __thread MCS node as null although the address is non-null at run
# time (verified in gdb, absent at -O0 and under clang).
SANITIZE_CFLAGS := -O1 -g -DSPINLOCK_DEBUG $(COMMON_CFLAGS) -ftls-model=local-exec \
                   -fsanitize=address,undefined -fno-sanitize-recover=all
SANITIZE_LDFLAGS := -fsanitize=address,undefined

# Source-navigation databases refreshed by 'make all': the clangd compilation
# database, a Universal Ctags tags file, and the cscope index. Missing tools are
# skipped with a notice rather than failing the build.
COMPDB := compile_commands.json
TAGS_FILE := tags
CSCOPE_FILES := cscope.files
CSCOPE_DB := cscope.out
CTAGS ?= ctags
CSCOPE ?= cscope
CLANGD ?= clangd
CLANG_TIDY ?= clang-tidy

.PHONY: all release trace check-binaries check sanitize index compdb tags cscope \
        lsp-check tidy clean distclean FORCE

all: release trace check-binaries index

release: $(TARGET_RELEASE)

trace: $(TARGET_TRACE)

check-binaries: $(TARGET_CHECK_RELEASE) $(TARGET_CHECK_TRACE)

$(TARGET_RELEASE): $(LIB_SRCS) $(BENCH_SRCS) $(HDRS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(RELEASE_CFLAGS) $(LIB_SRCS) $(BENCH_SRCS) -o $@ $(HARDEN_LDFLAGS) $(LDLIBS)

$(TARGET_TRACE): $(LIB_SRCS) $(BENCH_SRCS) $(HDRS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TRACE_CFLAGS) $(LIB_SRCS) $(BENCH_SRCS) -o $@ $(TRACE_LDFLAGS) $(HARDEN_LDFLAGS) $(LDLIBS)

$(TARGET_CHECK_RELEASE): $(LIB_SRCS) $(CHECK_SRCS) $(HDRS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(RELEASE_CFLAGS) $(LIB_SRCS) $(CHECK_SRCS) -o $@ $(HARDEN_LDFLAGS) $(LDLIBS)

$(TARGET_CHECK_TRACE): $(LIB_SRCS) $(CHECK_SRCS) $(HDRS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TRACE_CFLAGS) $(LIB_SRCS) $(CHECK_SRCS) -o $@ $(TRACE_LDFLAGS) $(HARDEN_LDFLAGS) $(LDLIBS)

$(TARGET_CHECK_SANITIZE): $(LIB_SRCS) $(CHECK_SRCS) $(HDRS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(SANITIZE_CFLAGS) $(LIB_SRCS) $(CHECK_SRCS) -o $@ $(SANITIZE_LDFLAGS) $(LDLIBS)

# Correctness suite: mutual-exclusion oracle under contention for every lock
# API, state-transition checks, and (trace build only) the SPINLOCK_DEBUG
# misuse aborts. A short benchmark pass confirms the atomic-count oracle too.
check: check-binaries release
	./$(TARGET_CHECK_RELEASE)
	./$(TARGET_CHECK_TRACE)
	./$(TARGET_RELEASE) -t 4 -l 0 -i 50000 | grep -E "Atomic Count" | grep -v OK && exit 1 || true

sanitize: $(TARGET_CHECK_SANITIZE)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./$(TARGET_CHECK_SANITIZE)

index: compdb tags cscope

compdb: $(COMPDB)

# One entry per translation unit with the release flags, so clangd parses every
# file (headers included, via the inferred command of the .c that includes
# them) exactly as the compiler does: gnu99, this target triple, these warnings.
# Regenerated on every run because the content depends on $(CC) (a cross
# compiler flips clangd to that target); the file is only replaced when the
# content changed, so clangd does not re-index for nothing.
$(COMPDB): $(SRCS) $(HDRS) Makefile FORCE
	@printf '[\n' > $@.tmp
	@first=1; for src in $(SRCS); do \
	    if [ $$first -eq 0 ]; then printf ',\n' >> $@.tmp; fi; first=0; \
	    printf '  {\n    "directory": "%s",\n    "file": "%s/%s",\n    "arguments": [' \
	        "$(abspath .)" "$(abspath .)" "$$src" >> $@.tmp; \
	    printf '"%s"' "$(CC)" >> $@.tmp; \
	    for flag in $(RELEASE_CFLAGS); do printf ', "%s"' "$$flag" >> $@.tmp; done; \
	    printf ', "-c", "%s"]\n  }' "$$src" >> $@.tmp; \
	done
	@printf '\n]\n' >> $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; echo "$@ unchanged"; else mv $@.tmp $@; echo "wrote $@"; fi

FORCE:

tags: $(SRCS) $(HDRS)
	@if command -v $(CTAGS) >/dev/null 2>&1; then \
	    $(CTAGS) --kinds-C=+px --fields=+iaSn --extras=+q -f $(TAGS_FILE) $(SRCS) $(HDRS) \
	        && echo "wrote $(TAGS_FILE)"; \
	else echo "ctags not found, skipping $(TAGS_FILE)"; fi

cscope: $(SRCS) $(HDRS)
	@if command -v $(CSCOPE) >/dev/null 2>&1; then \
	    printf '%s\n' $(SRCS) $(HDRS) > $(CSCOPE_FILES); \
	    $(CSCOPE) -b -q -k -i $(CSCOPE_FILES) && echo "wrote $(CSCOPE_DB)"; \
	else echo "cscope not found, skipping $(CSCOPE_DB)"; fi

# Ask clangd itself to parse every file with the compilation database and
# report the diagnostics an editor would show. Exits non-zero on any error.
lsp-check: $(COMPDB)
	@status=0; for src in $(SRCS) $(HDRS); do \
	    echo "clangd --check=$$src"; \
	    $(CLANGD) --check=$$src 2>&1 | grep -E '^[EW]\[|error:|warning:' | grep -vE 'tweak:|Failed to resolve URI' && status=1; \
	done; exit $$status

tidy: $(COMPDB)
	$(CLANG_TIDY) -p . $(SRCS)

# Build artifacts only.
clean:
	rm -rf $(BIN_DIR) build *.o

# clean + every debugger / profiler / tracer / cache / index file the workflow can drop.
distclean: clean
	rm -f core core.* *.core gdb.txt peda-session-*.txt
	rm -f vgcore.* callgrind.out.* cachegrind.out.* massif.out.* helgrind.out.* drd.out.*
	rm -f valgrind.log valgrind-*.log *.vgresult
	rm -f strace.out strace.log *.strace ltrace.out ltrace.log *.ltrace
	rm -rf uftrace.data uftrace.data.old
	rm -f perf.data perf.data.old flamegraph.svg gmon.out
	rm -rf __pycache__ .mypy_cache .ruff_cache .cache
	rm -rf CMakeFiles
	rm -f CMakeCache.txt cmake_install.cmake $(COMPDB) $(COMPDB).tmp
	rm -f $(TAGS_FILE) TAGS $(CSCOPE_FILES) $(CSCOPE_DB) cscope.in.out cscope.po.out

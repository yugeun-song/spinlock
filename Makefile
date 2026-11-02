ifeq ($(origin CC),default)
CC := gcc
endif

ifeq ($(origin JOBS),undefined)
JOBS := $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
endif
ifeq (,$(filter -j%,$(MAKEFLAGS)))
MAKEFLAGS += -j$(JOBS)
endif

ifneq (,$(word 2,$(MAKECMDGOALS)))

.PHONY: $(MAKECMDGOALS) run-goals-in-order

$(MAKECMDGOALS): run-goals-in-order
	@:

run-goals-in-order:
	@set -e; for goal in $(MAKECMDGOALS); do $(MAKE) --no-print-directory $$goal; done

else

BIN_ROOT := bin
LIB_SRCS := spinlock.c
BENCH_SRCS := spinlock_test.c main.c
CHECK_SRCS := spinlock_check.c
SRCS := $(LIB_SRCS) $(BENCH_SRCS) $(CHECK_SRCS)
HDRS := spinlock.h spinlock_test.h

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

TARGET_ARCHITECTURE := $(firstword $(subst -, ,$(TARGET_TRIPLE)))
COMPILER_FAMILY := $(if $(filter 1,$(shell printf '__clang__\n' | $(CC) -E -P -x c - 2>/dev/null)),clang,gcc)
COMPILER_VERSION := $(shell $(CC) --version 2>/dev/null | head -n 1)
TARGET_TAG := $(TARGET_ARCHITECTURE)-linux-$(COMPILER_FAMILY)

ifeq (,$(findstring -linux,$(TARGET_TRIPLE)))
ifneq (,$(filter-out clean distclean,$(or $(MAKECMDGOALS),all)))
$(error $(if $(TARGET_TRIPLE),CC=$(CC) targets '$(TARGET_TRIPLE)' but this project builds for Linux only,CC=$(CC) did not answer -dumpmachine; check that it is installed))
endif
endif

TARGET_BIN_DIR := $(BIN_ROOT)/$(TARGET_TAG)
RELEASE_DIR := $(TARGET_BIN_DIR)/release
DEBUG_DIR := $(TARGET_BIN_DIR)/debug
SANITIZE_DIR := $(TARGET_BIN_DIR)/sanitize

RELEASE_BENCH := $(RELEASE_DIR)/spinlock_test
RELEASE_CHECK := $(RELEASE_DIR)/spinlock_check
DEBUG_BENCH := $(DEBUG_DIR)/spinlock_test
DEBUG_CHECK := $(DEBUG_DIR)/spinlock_check
SANITIZE_CHECK := $(SANITIZE_DIR)/spinlock_check
BENCH_BINARIES := $(RELEASE_BENCH) $(DEBUG_BENCH)
CHECK_BINARIES := $(RELEASE_CHECK) $(DEBUG_CHECK) $(SANITIZE_CHECK)

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
RELEASE_LDFLAGS := $(HARDEN_LDFLAGS)
DEBUG_CFLAGS := -O0 -g3 -DSPINLOCK_DEBUG $(COMMON_CFLAGS) \
                -fno-inline -fno-inline-functions \
                -fno-optimize-sibling-calls
DEBUG_LDFLAGS := -rdynamic $(HARDEN_LDFLAGS)
# -ftls-model=local-exec is valid here (every thread-local lives in this
# executable) and sidesteps a GCC 16 -fsanitize=null false positive: with the
# initial-exec or dynamic TLS models its inlined check reports the address of
# the extern __thread MCS node as null although the address is non-null at run
# time (verified in gdb, absent at -O0 and under clang).
SANITIZE_CFLAGS := -O1 -g -DSPINLOCK_DEBUG $(COMMON_CFLAGS) -ftls-model=local-exec \
                   -fsanitize=address,undefined -fno-sanitize-recover=all
SANITIZE_LDFLAGS := -fsanitize=address,undefined

RELEASE_BENCH_COMMAND = $(CC) $(RELEASE_CFLAGS) $(LIB_SRCS) $(BENCH_SRCS) -o $(RELEASE_BENCH) \
                        $(RELEASE_LDFLAGS) $(LDLIBS)
RELEASE_CHECK_COMMAND = $(CC) $(RELEASE_CFLAGS) $(LIB_SRCS) $(CHECK_SRCS) -o $(RELEASE_CHECK) \
                        $(RELEASE_LDFLAGS) $(LDLIBS)
DEBUG_BENCH_COMMAND = $(CC) $(DEBUG_CFLAGS) $(LIB_SRCS) $(BENCH_SRCS) -o $(DEBUG_BENCH) \
                      $(DEBUG_LDFLAGS) $(LDLIBS)
DEBUG_CHECK_COMMAND = $(CC) $(DEBUG_CFLAGS) $(LIB_SRCS) $(CHECK_SRCS) -o $(DEBUG_CHECK) \
                      $(DEBUG_LDFLAGS) $(LDLIBS)
SANITIZE_CHECK_COMMAND = $(CC) $(SANITIZE_CFLAGS) $(LIB_SRCS) $(CHECK_SRCS) -o $(SANITIZE_CHECK) \
                         $(SANITIZE_LDFLAGS) $(LDLIBS)

ifneq (,$(filter all,$(or $(MAKECMDGOALS),all)))
SANITIZER_LINK_SUPPORTED := $(shell probe_output=$$(mktemp) || exit 0; \
    printf 'int main(void) { return 0; }\n' | \
    $(CC) $(SANITIZE_LDFLAGS) -x c - -o "$$probe_output" >/dev/null 2>&1 && echo yes; \
    rm -f "$$probe_output")
endif

shell_quote = '$(subst ','\'',$(1))'
strings_equal = $(and $(findstring x$(1),x$(2)),$(findstring x$(2),x$(1)))
command_stamp_path = $(dir $(1)).$(notdir $(1)).cmd
command_record = $(strip $(COMPILER_VERSION) | $(1))
force_if_command_changed = $(if $(call strings_equal,$(strip $(file <$(call command_stamp_path,$(1)))),$(call command_record,$(2))),,FORCE)

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

.PHONY: all release debug check sanitize index compdb tags cscope lsp-check tidy \
        clean distclean FORCE

all: $(RELEASE_BENCH) $(RELEASE_CHECK) $(DEBUG_BENCH) $(DEBUG_CHECK) \
     $(if $(SANITIZER_LINK_SUPPORTED),$(SANITIZE_CHECK)) index
	$(if $(SANITIZER_LINK_SUPPORTED),,@echo "skipped $(SANITIZE_CHECK): $(CC) cannot link $(SANITIZE_LDFLAGS)")
	@echo "programs are in $(TARGET_BIN_DIR)/"

release: $(RELEASE_BENCH) $(RELEASE_CHECK)

debug: $(DEBUG_BENCH) $(DEBUG_CHECK)

$(RELEASE_BENCH): BUILD_COMMAND = $(RELEASE_BENCH_COMMAND)
$(RELEASE_CHECK): BUILD_COMMAND = $(RELEASE_CHECK_COMMAND)
$(DEBUG_BENCH): BUILD_COMMAND = $(DEBUG_BENCH_COMMAND)
$(DEBUG_CHECK): BUILD_COMMAND = $(DEBUG_CHECK_COMMAND)
$(SANITIZE_CHECK): BUILD_COMMAND = $(SANITIZE_CHECK_COMMAND)

$(BENCH_BINARIES): $(LIB_SRCS) $(BENCH_SRCS) $(HDRS)
$(CHECK_BINARIES): $(LIB_SRCS) $(CHECK_SRCS) $(HDRS)
$(RELEASE_BENCH): $(call force_if_command_changed,$(RELEASE_BENCH),$(RELEASE_BENCH_COMMAND))
$(RELEASE_CHECK): $(call force_if_command_changed,$(RELEASE_CHECK),$(RELEASE_CHECK_COMMAND))
$(DEBUG_BENCH): $(call force_if_command_changed,$(DEBUG_BENCH),$(DEBUG_BENCH_COMMAND))
$(DEBUG_CHECK): $(call force_if_command_changed,$(DEBUG_CHECK),$(DEBUG_CHECK_COMMAND))
$(SANITIZE_CHECK): $(call force_if_command_changed,$(SANITIZE_CHECK),$(SANITIZE_CHECK_COMMAND))

$(BENCH_BINARIES) $(CHECK_BINARIES):
	@mkdir -p $(@D)
	$(BUILD_COMMAND)
	@printf '%s\n' $(call shell_quote,$(call command_record,$(BUILD_COMMAND))) > $(call command_stamp_path,$@)

# Correctness suite: mutual-exclusion oracle under contention for every lock
# API, state-transition checks, and (debug build only) the SPINLOCK_DEBUG
# misuse aborts. A short benchmark pass confirms the atomic-count oracle too.
check: $(RELEASE_CHECK) $(DEBUG_CHECK) $(RELEASE_BENCH)
	./$(RELEASE_CHECK)
	./$(DEBUG_CHECK)
	./$(RELEASE_BENCH) -t 4 -l 0 -i 50000 | grep -E "Atomic Count" | grep -v OK && exit 1 || true

sanitize: $(SANITIZE_CHECK)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./$(SANITIZE_CHECK)

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
	rm -rf $(BIN_ROOT) build *.o

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

endif

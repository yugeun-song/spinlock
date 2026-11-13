.NOTPARALLEL:

GOALS := all debug sanitize release index clean-index clean distclean unused check tidy lsp-check
CC_WORD := $(notdir $(firstword $(CC)))
ifeq ($(origin CC),default)
PRESET ?=
else ifneq ($(filter gcc clang,$(CC_WORD)),)
PRESET ?= $(shell uname -m)-linux-$(CC_WORD)
else ifneq ($(filter %-linux-gnu-gcc,$(CC_WORD)),)
PRESET ?= $(firstword $(subst -, ,$(CC_WORD)))-linux-gcc
endif
BUILD = cmake -D BUILD_SCRIPT=make -P build.cmake --

.PHONY: $(GOALS) help

$(GOALS):
	@$(BUILD) $@ $(PRESET) $(if $(SANITIZE),SANITIZE=$(SANITIZE))

help:
	@printf '%s\n' \
	    'usage: make [verb ...] [PRESET=<target>] [CC=gcc|clang] [SANITIZE=<list>]' \
	    '' \
	    '  all          debug and sanitize (default)' \
	    '  debug        Debug: -O0, full debug info, -pg, no injected hardening, no PIE' \
	    '  sanitize     Sanitize: Debug with -fsanitize=<list>, address by default' \
	    '  release      Release: -O2, hardened, LTO' \
	    '  index        compile_commands.json, tags and cscope.out in the source root' \
	    '  clean-index  remove the index files' \
	    '  clean        remove every build product; configured trees stay configured' \
	    '  distclean    clean, clean-index, build/, tool outputs and caches' \
	    '  unused       unused diagnostics of every source (clang) and unused functions (cppcheck)' \
	    '  check        build every config the target can link and run its tests' \
	    '  tidy         clang-tidy over every source with the Debug flags' \
	    '  lsp-check    let clangd parse every source and header and print its errors' \
	    ''
	@$(BUILD) -h | sed -n '/^Available targets/,$$p'

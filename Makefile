.NOTPARALLEL:

GOALS := all debug sanitize release index clean-index clean distclean unused check tidy lsp-check help
CC_WORD := $(notdir $(firstword $(CC)))
ifeq ($(origin CC),default)
PRESET ?=
else ifneq ($(filter gcc clang,$(CC_WORD)),)
PRESET ?= $(shell uname -m)-linux-$(CC_WORD)
else ifneq ($(filter %-linux-gnu-gcc,$(CC_WORD)),)
PRESET ?= $(firstword $(subst -, ,$(CC_WORD)))-linux-gcc
endif
BUILD = cmake -D BUILD_SCRIPT=make -P build.cmake --

.PHONY: $(GOALS)

$(GOALS):
	@$(BUILD) $@ $(PRESET) $(if $(PROFILE),PROFILE=$(PROFILE)) $(if $(SANITIZE),SANITIZE=$(SANITIZE))

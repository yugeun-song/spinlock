.NOTPARALLEL:

GOALS := all debug sanitize release index clean-index clean distclean unused check tidy clangd-check help
CC_NAME := $(notdir $(firstword $(CC)))
ifeq ($(origin CC),default)
PRESET ?=
else ifneq ($(filter gcc clang,$(CC_NAME)),)
PRESET ?= $(shell uname -m)-linux-$(CC_NAME)
else ifneq ($(filter %-linux-gnu-gcc,$(CC_NAME)),)
PRESET ?= $(firstword $(subst -, ,$(CC_NAME)))-linux-gcc
endif
BUILD = cmake -D BUILD_SCRIPT=make -P build.cmake --

.PHONY: $(GOALS)

$(GOALS):
	@$(BUILD) $@ $(PRESET) $(if $(PROFILE),PROFILE=$(PROFILE)) $(if $(SANITIZE),SANITIZE=$(SANITIZE))

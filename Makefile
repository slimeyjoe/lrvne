# SPDX-License-Identifier: GPL-3.0-or-later
# lrvne - GNU Make 4
#
#   make [TARGET=host|sdl3|...]     build the demo engine (examples/demo) on the library
#   make test [SAN=1]               build and run the unit tests (host target)
#   make check                      test + the tools' tests + the leak check
#   make clean
#
# Engines do not use this file: they include mk/lrvne.mk (see there).

LRVNE    := .
TARGET   ?= host
BUILD    := build/$(TARGET)
APP_NAME := lrvdemo
APP_SRCS := $(sort $(wildcard examples/demo/*.c))
APP_INCLUDES := -Iexamples/demo

.DEFAULT_GOAL := all
include mk/lrvne.mk

.PHONY: all test check clean
all: $(EXE)

# ---- unit tests: each tests/test_*.c is a program linked with the library (no backend) ------
TEST_SRCS := $(sort $(wildcard tests/test_*.c))
TEST_EXES := $(patsubst tests/%.c,$(BUILD)/tests/%$(EXESUF),$(TEST_SRCS))
DEPS      += $(patsubst tests/%.c,$(BUILD)/tests/%.d,$(TEST_SRCS))
-include $(patsubst tests/%.c,$(BUILD)/tests/%.d,$(TEST_SRCS))

$(BUILD)/tests/%$(EXESUF): $(BUILD)/tests/%.o $(LRVNE_LIB_OBJS)
	@echo "  LINK  $@"
	$(Q)$(LRVNE_LD) $(OPT) -o $@ $< $(LRVNE_LIB_OBJS) $(LDLIBS)

test: $(TEST_EXES)
	$(Q)for t in $(TEST_EXES); do ./$$t || exit 1; done

check: test
	$(Q)python3 -I -m unittest discover -s tools/tests -t tools
	$(Q)scripts/check-leaks.sh tree

clean:
	rm -rf build

# SPDX-License-Identifier: GPL-3.0-or-later
# lrvne - GNU Make 4
#
#   make [TARGET=host|sdl3|...]     build the demo engine (examples/demo) on the library
#   make test [SAN=1]               build and run the unit tests, then the demo headless
#                                   (tests/smoke.sh; host target)
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

# ---- unit tests: each tests/test_*.c is a program linked with the library and with what the
# backends share (backends/common, backends/control), but with no backend -----------------------
TEST_SRCS := $(sort $(wildcard tests/test_*.c))
TEST_EXES := $(patsubst tests/%.c,$(BUILD)/tests/%$(EXESUF),$(TEST_SRCS))
TEST_OBJS := $(LRVNE_LIB_OBJS) $(filter $(BUILD)/$(LRVNE)/backends/common/% \
               $(BUILD)/$(LRVNE)/backends/control/%,$(LRVNE_BACKEND_OBJS))
DEPS      += $(patsubst tests/%.c,$(BUILD)/tests/%.d,$(TEST_SRCS))
-include $(patsubst tests/%.c,$(BUILD)/tests/%.d,$(TEST_SRCS))

$(BUILD)/tests/%$(EXESUF): $(BUILD)/tests/%.o $(TEST_OBJS)
	@echo "  LINK  $@"
	$(Q)$(LRVNE_LD) $(OPT) -o $@ $< $(TEST_OBJS) $(LDLIBS)

# then the demo engine is run headless (tests/smoke.sh), which needs the host target's backend
test: $(TEST_EXES) $(EXE)
	$(Q)for t in $(TEST_EXES); do ./$$t || exit 1; done
ifeq ($(TARGET),host)
	$(Q)tests/smoke.sh $(EXE)
endif

check: test
	$(Q)python3 -I -m unittest discover -s tools/tests -t tools
	$(Q)scripts/check-leaks.sh tree

clean:
	rm -rf build

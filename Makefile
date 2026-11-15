# SPDX-License-Identifier: GPL-3.0-or-later
# lrvne - GNU Make 4
#
#   make [TARGET=host|sdl3|amiga|warpos|aros]
#                                   build the demo engine (examples/demo) on the library; the
#                                   targets are listed in mk/lrvne.mk
#   make test [SAN=1]               build and run the unit tests, then the demo headless
#                                   (tests/smoke.sh; host target). SAN=1: with the sanitizers,
#                                   in a build directory of its own (build/<target>-san)
#   make check                      test + the tools' tests + the leak check
#   make opn68k                     the 68k assembly FM loop against its C reference on an
#                                   emulated 68040 (tests/opn68k.sh says what it needs)
#   make clean
#
# Switches (0/1, see mk/lrvne.mk): LRVNE_YMFM, LRVNE_OPUS, LRVNE_CONTROL. Changing one rebuilds
# what depends on it.
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

.PHONY: all test check opn68k clean
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

# the tools (tools/) have their own tests; a checkout without them skips the step
check: test
	$(Q)if [ -d tools/tests ]; then python3 -I -m unittest discover -s tools/tests -t tools; \
	    else echo "check: no tools/tests, skipping the tools' tests"; fi
	$(Q)scripts/check-leaks.sh tree

# cross tools only: the same whatever TARGET is
opn68k:
	$(Q)tests/opn68k.sh build/opn68k

clean:
	rm -rf build

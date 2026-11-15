# SPDX-License-Identifier: GPL-3.0-or-later
# lrvne - the make fragment an engine includes to compile the library into itself.
#
# lrvne is not built as a library of its own: its sources are compiled with the engine's, by
# the engine's compiler and flags, into the engine's executable. An engine's Makefile sets
#
#   LRVNE   := lrvne              where this repository is (a submodule of the engine)
#   TARGET  ?= host               host | sdl3 | amiga | warpos | aros  (mk/target/<TARGET>.mk)
#   BUILD   := ../build/$(TARGET) where objects go (see below)
#   APP_NAME, APP_SRCS, APP_INCLUDES, APP_DEFS   the engine's own
#
# and includes this file, which defines the toolchain of the target, the lists of lrvne's
# sources for it, the compile rules and `$(EXE)`. Switches (0/1):
#
#   LRVNE_YMFM     the ymfm YM2203 core (C++); the C core is always there
#   LRVNE_OPUS     the Opus decoder for voice clips
#   LRVNE_CONTROL  the test automation protocol in the backend
#
# Objects built in different ways never meet:
#   - This file appends $(LRVNE_BUILD_SUFFIX) to BUILD. A target sets it for builds whose
#     objects cannot be linked with the ordinary ones: "-san" with SAN=1 on the hosted targets,
#     so `make` and `make SAN=1` use build/host and build/host-san. Use $(BUILD) only after the
#     include. (A BUILD given on make's command line is taken as it is.)
#   - $(BUILD)/.lrvne-flags records the compilers and flags; every object depends on it, so
#     changing a switch or a flag rebuilds what was compiled with the old ones.
#
# Everything here is prefixed LRVNE_ except the conventional CC, CXX, CFLAGS, LDLIBS, BUILD,
# EXE, Q and V.

LRVNE         ?= lrvne
TARGET        ?= host
BUILD         ?= build/$(TARGET)
LRVNE_YMFM    ?= 1
LRVNE_OPUS    ?= 0
LRVNE_CONTROL ?= 1

ifeq ($(V),1)
Q :=
else
Q := @
endif

# ---- the target's toolchain: CC, CXX, LRVNE_BACKEND_DIRS, LRVNE_TARGET_*, COMPILE/LINK ------
LRVNE_BUILD_SUFFIX :=
include $(LRVNE)/mk/target/$(TARGET).mk
BUILD := $(BUILD)$(LRVNE_BUILD_SUFFIX)

# ---- sources --------------------------------------------------------------------------------
LRVNE_LIB_SRCS := $(sort $(wildcard $(LRVNE)/src/*/*.c $(LRVNE)/src/*/*/*.c))
LRVNE_CXX_SRCS :=
LRVNE_DEFS     :=
ifeq ($(LRVNE_YMFM),1)
LRVNE_CXX_SRCS += $(sort $(wildcard $(LRVNE)/third_party/ymfm/*.cpp $(LRVNE)/src/*/*.cpp))
LRVNE_DEFS     += -DLRV_HAVE_YMFM
endif
ifeq ($(LRVNE_OPUS),1)
include $(LRVNE)/mk/opus.mk
LRVNE_DEFS     += -DLRV_HAVE_OPUS
endif

LRVNE_BACKEND_SRCS := $(sort $(foreach d,$(LRVNE_BACKEND_DIRS) $(LRVNE)/backends/common,$(wildcard $(d)/*.c)))
ifeq ($(LRVNE_CONTROL),1)
LRVNE_BACKEND_SRCS += $(sort $(wildcard $(LRVNE)/backends/control/*.c))
LRVNE_DEFS         += -DLRV_HAVE_CONTROL
endif

LRVNE_INCLUDES := -I$(LRVNE)/include -I$(LRVNE)/backends/common -I$(LRVNE)/backends/control \
                  $(addprefix -I,$(LRVNE_BACKEND_DIRS))

# ---- objects and rules ----------------------------------------------------------------------
lrvne_obj = $(patsubst %,$(BUILD)/%.o,$(basename $(1)))

LRVNE_LIB_OBJS     := $(call lrvne_obj,$(LRVNE_LIB_SRCS) $(LRVNE_CXX_SRCS) $(LRVNE_EXTRA_SRCS))
LRVNE_BACKEND_OBJS := $(call lrvne_obj,$(LRVNE_BACKEND_SRCS))
APP_OBJS           := $(call lrvne_obj,$(APP_SRCS))
OBJS               := $(APP_OBJS) $(LRVNE_LIB_OBJS) $(LRVNE_BACKEND_OBJS)
DEPS               := $(OBJS:.o=.d)
INCLUDES           := $(APP_INCLUDES) $(LRVNE_INCLUDES)
CFLAGS             += $(LRVNE_DEFS) $(APP_DEFS)

EXE ?= $(BUILD)/$(APP_NAME)$(EXESUF)

$(EXE): $(OBJS)
	@echo "  LINK  $@"
	$(Q)$(LINK)

# what the objects were compiled with; rewritten, and so newer than them, only when it changes
LRVNE_FLAGS       := $(strip $(CC) $(CXX) $(CFLAGS) $(OPT) $(INCLUDES) $(LRVNE_OPUS_CFLAGS))
LRVNE_FLAGS_FILE  := $(BUILD)/.lrvne-flags
lrvne_flags_print  = printf '%s\n' '$(subst ','\'',$(LRVNE_FLAGS))'

$(LRVNE_FLAGS_FILE): lrvne-always
	@mkdir -p $(dir $@)
	@$(lrvne_flags_print) | cmp -s - $@ || $(lrvne_flags_print) >$@

# makes the recipe above run every time (declared phony: with .SECONDARY below a file that
# merely does not exist would not)
.PHONY: lrvne-always
lrvne-always:

$(BUILD)/%.o: %.c $(LRVNE_FLAGS_FILE)
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(Q)$(COMPILE_C)

$(BUILD)/%.o: %.cpp $(LRVNE_FLAGS_FILE)
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	$(Q)$(COMPILE_CXX)

$(BUILD)/%.o: %.s $(LRVNE_FLAGS_FILE)
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	$(Q)$(COMPILE_S)

.SECONDARY:
-include $(DEPS)

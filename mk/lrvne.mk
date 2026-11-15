# SPDX-License-Identifier: GPL-3.0-or-later
# lrvne - the make fragment an engine includes to compile the library into itself.
#
# lrvne is not built as a library of its own: its sources are compiled with the engine's, by
# the engine's compiler and flags, into the engine's executable. An engine's Makefile sets
#
#   LRVNE   := lrvne              where this repository is (a submodule of the engine)
#   TARGET  ?= host               host | sdl3 | amiga | warpos | aros  (mk/target/<TARGET>.mk)
#   BUILD   := ../build/$(TARGET) where objects go
#   APP_NAME, APP_SRCS, APP_INCLUDES, APP_DEFS   the engine's own
#
# and includes this file, which defines the toolchain of the target, the lists of lrvne's
# sources for it, the compile rules and `$(EXE)`. Switches (0/1):
#
#   LRVNE_YMFM     the ymfm YM2203 core (C++); the C core is always there
#   LRVNE_OPUS     the Opus decoder for voice clips
#   LRVNE_CONTROL  the test automation protocol in the backend
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
include $(LRVNE)/mk/target/$(TARGET).mk

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

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(Q)$(COMPILE_C)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	$(Q)$(COMPILE_CXX)

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	$(Q)$(COMPILE_S)

.SECONDARY:
-include $(DEPS)

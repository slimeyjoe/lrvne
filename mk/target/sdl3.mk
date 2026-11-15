# SPDX-License-Identifier: GPL-3.0-or-later
# SDL3 build: Linux, macOS and whatever else SDL3 runs on.
CC      ?= cc
CXX     ?= c++
EXESUF  :=
LRVNE_BACKEND_DIRS := $(LRVNE)/backends/sdl3
# release archives: <name>-<version>-sdl3-<os>-<arch>
LRVNE_DIST_NAME := sdl3-$(shell uname -s | tr A-Z a-z)-$(shell uname -m)

LRVNE_SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
LRVNE_SDL_LIBS   := $(shell pkg-config --libs sdl3)

LRVNE_CSTD := -std=c99
LRVNE_WARN := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla
OPT     ?= -O2 -g
ifeq ($(SAN),1)
OPT     := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
endif

CFLAGS  := $(LRVNE_CSTD) $(LRVNE_WARN) $(OPT) $(LRVNE_SDL_CFLAGS) -D_DEFAULT_SOURCE -DLRV_TARGET_SDL3
LDLIBS  := $(LRVNE_SDL_LIBS) -lm

COMPILE_C   = $(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<
COMPILE_CXX = $(CXX) -std=c++14 $(OPT) -I$(LRVNE)/third_party $(INCLUDES) $(LRVNE_DEFS) -MMD -MP -c -o $@ $<
LRVNE_LD    = $(if $(strip $(LRVNE_CXX_SRCS)),$(CXX),$(CC))
LINK        = $(LRVNE_LD) $(OPT) -o $@ $(OBJS) $(LDLIBS)

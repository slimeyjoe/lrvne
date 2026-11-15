# SPDX-License-Identifier: GPL-3.0-or-later
# Host build: the native compiler and the null backend. For tests and tools.
#   SAN=1 turns the sanitizers on.
CC      ?= cc
CXX     ?= c++
EXESUF  :=
LRVNE_BACKEND_DIRS := $(LRVNE)/backends/null

LRVNE_CSTD := -std=c99
LRVNE_WARN := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
              -Wconversion -Wno-sign-conversion -Wvla
OPT     ?= -O2 -g
ifeq ($(SAN),1)
OPT     := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
endif

CFLAGS  := $(LRVNE_CSTD) $(LRVNE_WARN) $(OPT) -D_POSIX_C_SOURCE=200809L -DLRV_TARGET_HOST
LDLIBS  := -lm

COMPILE_C   = $(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<
COMPILE_CXX = $(CXX) -std=c++14 $(OPT) -I$(LRVNE)/third_party $(INCLUDES) $(LRVNE_DEFS) -MMD -MP -c -o $@ $<
# the C++ driver links when C++ objects are in (ymfm), the C driver otherwise
LRVNE_LD    = $(if $(strip $(LRVNE_CXX_SRCS)),$(CXX),$(CC))
LINK        = $(LRVNE_LD) $(OPT) -o $@ $(OBJS) $(LDLIBS)

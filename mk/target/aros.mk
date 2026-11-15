# SPDX-License-Identifier: GPL-3.0-or-later
# AROS (x86_64, ABIv11) with the AROS cross gcc. backends/amiga/README.md "AROS".
#   make TARGET=aros
# x86_64-aros-gcc is expected on the PATH and AROS_SDK in the environment: the SDK's
# Development directory, which becomes the compiler's sysroot.
# The backend's parts: the shared core, the exec system glue and the AROS flavour; no chipset
# (sound through AHI, chunky video upload).
# (make's built-in CC=cc would win over ?=: replace only that default)
ifeq ($(origin CC),default)
CC      := x86_64-aros-gcc
endif
CXX     :=
STRIP   ?= x86_64-aros-strip
EXESUF  :=
LRVNE_BACKEND_DIRS := $(addprefix $(LRVNE)/backends/amiga/,core exec aros)
# <name>-<version>-aros-x86_64.lha with a stripped executable
LRVNE_DIST_NAME    := aros-x86_64
LRVNE_DIST_STRIP   := $(STRIP) --strip-unneeded
include $(LRVNE)/mk/amiga.mk

LRVNE_CSTD := -std=gnu99        # the AROS headers use the asm keyword
LRVNE_WARN := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla \
              -Wno-unused-parameter -Wno-pointer-sign   # STRPTR is unsigned char *
OPT     ?= -O2
LRVNE_SYSROOT := $(if $(AROS_SDK),--sysroot=$(AROS_SDK))
CFLAGS  := $(LRVNE_CSTD) $(LRVNE_WARN) $(OPT) $(LRVNE_SYSROOT) -DLRV_AMIGA_AROS
LDLIBS  := -lm

# the Opus decoder (LRVNE_OPUS=1; mk/opus.mk): variable-length arrays for libopus's scratch
# memory (the AROS SDK's <alloca.h> does not declare alloca under these flags)
LRVNE_OPUS_CSTD := -std=gnu99 $(LRVNE_SYSROOT)
LRVNE_OPUS_DEFS := -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DVAR_ARRAYS

COMPILE_C   = $(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<
COMPILE_CXX = $(error C++ is not used in the AROS build)
LRVNE_LD    = $(CC)
LINK        = $(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

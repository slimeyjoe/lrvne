# SPDX-License-Identifier: GPL-3.0-or-later
# WarpOS (PowerPC cards, WarpUP 5.x) with vbcc 0.9h: the Amiga backend compiled for the PPC.
# backends/amiga/README.md "WarpOS".
#   make TARGET=warpos
# The executable is a WarpOS (EHF) file: 68k startup + PPC code + a few 68k objects for the
# parts that run on the 68k (LRVNE_M68K_SRCS below). `vc` with its warpos and aos68k
# configurations is expected on the PATH, the WarpUP headers where the warpos configuration
# looks for them.
VC      ?= vc +warpos
VC68K   ?= vc +aos68k
CC      := $(VC)
CXX     :=
EXESUF  :=
# the backend's parts: the shared core, the classic chipset, the WarpOS flavour with its
# system glue (instead of exec/)
LRVNE_BACKEND_DIRS := $(addprefix $(LRVNE)/backends/amiga/,core chipset warpos)
LRVNE_DIST_NAME    := warpos
include $(LRVNE)/mk/amiga.mk
LRVNE_AUTODEPS := 0

# -c99; -O2 -speed; LRV_AMIGA_WARPOS is the flavour (backends/amiga/core/amcompat.h).
# No 68k assembly: the OPN core renders with its C loop. The OS headers are read under
# #pragma amiga-align (backends/amiga/core/os_begin.h), never the whole program.
OPT     ?= -O2 -speed
# -dontwarn: 153 is "statement has no effect" for every (void)parameter; 368 is half of what
# this target's own <string.h> and <stdlib.h> draw in every file (their inline versions of
# strlen, abs, ... redeclare the prototypes with registers). The other half, warning 68
# ("redeclaration of var ... with new type") for those same lines, cannot be turned off.
LRVNE_VBCC_QUIET := -dontwarn=153 -dontwarn=368
CFLAGS  := -c99 $(OPT) $(LRVNE_VBCC_QUIET) -DLRV_AMIGA_WARPOS
# the PPC has an FPU: libm, not the 68k build's software floating point
LDLIBS  := -lamiga -lm

# the Opus decoder (LRVNE_OPUS=1; mk/opus.mk): vbcc has no alloca, so libopus's own scratch
# stack (120 KB, allocated at the first clip)
LRVNE_OPUS_CSTD := -c99 $(LRVNE_VBCC_QUIET)
LRVNE_OPUS_DEFS := -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DNONTHREADSAFE_PSEUDOSTACK

COMPILE_C   = $(VC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
COMPILE_CXX = $(error C++ is not available in the WarpOS build)
LRVNE_LD    = $(VC)
LINK        = $(VC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

# 68k objects linked into the PPC executable: the once-per-frame OS call, the Paula interrupt
# handler, the layout guard's reference table. They are ordinary sources of their parts, so
# mk/lrvne.mk lists them; for these objects the compile command is the 68k compiler's (BUILD
# is final here: this target adds no suffix to it).
LRVNE_M68K_SRCS   := $(addprefix $(LRVNE)/backends/amiga/,core/frameos.c chipset/paula_irq.c \
                       warpos/oslayout68k.c)
LRVNE_M68K_CFLAGS := -c99 -O2 -cpu=68020 -dontwarn=153 -DLRV_AMIGA_WARPOS
$(patsubst %,$(BUILD)/%.o,$(basename $(LRVNE_M68K_SRCS))): \
    COMPILE_C = $(VC68K) $(LRVNE_M68K_CFLAGS) $(LRVNE_DEFS) $(APP_DEFS) $(INCLUDES) -c -o $@ $<

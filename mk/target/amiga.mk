# SPDX-License-Identifier: GPL-3.0-or-later
# AmigaOS 3.x on the 68k (68020 or better, by default a 68040) with vbcc 0.9h and vasm: native
# AGA/ECS display or a Picasso96 graphics card, Paula or AHI. backends/amiga/README.md.
#   make TARGET=amiga [CPU=68040] [LRVNE_OPN_ASM=0]
# The compiler driver `vc` with its aos68k configuration and `vasmm68k_mot` are expected on
# the PATH, with VBCC and the NDK set up as the vbcc installation says; nothing here names a
# directory.
VC      ?= vc +aos68k
VASM    ?= vasmm68k_mot
CPU     ?= 68040
CC      := $(VC)
CXX     :=
EXESUF  :=
# the backend's parts: the shared core, the exec system glue, the classic chipset (Paula,
# planar chip RAM), the AmigaOS 3.x flavour
LRVNE_BACKEND_DIRS := $(addprefix $(LRVNE)/backends/amiga/,core exec chipset os3)
LRVNE_DIST_NAME    := amiga
include $(LRVNE)/mk/amiga.mk
# vbcc writes no dependency files: every object depends on every header (mk/lrvne.mk)
LRVNE_AUTODEPS := 0

# -c99: C99 mode; -speed: optimise for speed; -cpu: code generation target.
# No -sd (small data): an engine's static data may exceed 64 KB.
OPT     ?= -O2 -speed
# the light OPN core's per-sample render loop in 68020+ assembly (src/snd/opn_lite_68k.s, bit
# for bit the C loop; LRVNE_OPN_ASM=0 builds the C loop instead)
LRVNE_OPN_ASM ?= 1
ifeq ($(LRVNE_OPN_ASM),1)
LRVNE_TARGET_DEFS := -DLRV_OPN_ASM_68K
LRVNE_EXTRA_SRCS  += $(LRVNE)/src/snd/opn_lite_68k.s
endif

# -dontwarn=153: "statement has no effect" for every (void)parameter
CFLAGS  := -c99 -cpu=$(CPU) $(OPT) -dontwarn=153 -DLRV_AMIGA_OS3 $(LRVNE_TARGET_DEFS)
# -lmieee: software floating point (an engine's offline helpers may use double)
LDLIBS  := -lamiga -lmieee

# the Opus decoder (LRVNE_OPUS=1; mk/opus.mk): vbcc has no alloca, so libopus's own scratch
# stack (120 KB, allocated at the first clip). It decodes far slower than real time on a
# 68040: for a 68060 or an emulator; delta-coded and PCM clips need none of it.
LRVNE_OPUS_CSTD := -c99 -cpu=$(CPU) -dontwarn=153
LRVNE_OPUS_DEFS := -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DNONTHREADSAFE_PSEUDOSTACK

COMPILE_C   = $(VC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
COMPILE_CXX = $(error C++ is not available in the Amiga build)
COMPILE_S   = $(VASM) -Fhunk -quiet -o $@ $<
LRVNE_LD    = $(VC)
LINK        = $(VC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

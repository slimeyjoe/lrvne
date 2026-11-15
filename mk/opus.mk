# SPDX-License-Identifier: GPL-3.0-or-later
# The Opus decoder for voice clips (src/codec/vdec_opus.c), included by mk/lrvne.mk when
# LRVNE_OPUS=1.
#
# third_party/opus is libopus 1.6.1 (https://opus-codec.org, BSD licence: its COPYING), decoder
# only, fixed point: only the files the decoder links are there (the decoder's objects of a
# full static build of CELT, SILK and SILK fixed).
# Local change: celt/fixed_generic.h has QCONST16/QCONST32 variants for vbcc (#ifdef __VBCC__),
# whose constant folding gets two of the originals' conversions wrong.
#
# libopus is compiled with its own flags instead of the library's (no warnings of ours, its
# own configuration macros). A target whose compiler differs sets, before mk/lrvne.mk includes
# this file (in mk/target/<TARGET>.mk):
#
#   LRVNE_OPUS_DEFS   libopus's configuration. The default uses alloca for its scratch memory,
#                     which needs alloca() declared: HAVE_ALLOCA_H makes libopus include
#                     <alloca.h> (glibc's <stdlib.h> declares it only outside strict C99, and
#                     an undeclared alloca truncates the pointer on 64-bit targets). A target
#                     without alloca uses -DVAR_ARRAYS or -DNONTHREADSAFE_PSEUDOSTACK in its
#                     place.
#   LRVNE_OPUS_CSTD   the language standard option (vbcc: -c99)

LRVNE_OPUS_DIR   := $(LRVNE)/third_party/opus
LRVNE_OPUS_SRCS  := $(sort $(wildcard $(LRVNE_OPUS_DIR)/celt/*.c $(LRVNE_OPUS_DIR)/silk/*.c \
                                      $(LRVNE_OPUS_DIR)/src/*.c))
LRVNE_OPUS_DEFS  ?= -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DUSE_ALLOCA -DHAVE_ALLOCA_H
LRVNE_OPUS_CSTD  ?= -std=c99
LRVNE_OPUS_CFLAGS = $(LRVNE_OPUS_CSTD) $(OPT) $(LRVNE_OPUS_DEFS) -I$(LRVNE_OPUS_DIR)/include \
                    -I$(LRVNE_OPUS_DIR)/celt -I$(LRVNE_OPUS_DIR)/silk -I$(LRVNE_OPUS_DIR)/src

# into the library's objects ...
LRVNE_EXTRA_SRCS += $(LRVNE_OPUS_SRCS)
# ... compiled with libopus's flags (the object names as mk/lrvne.mk derives them)
$(patsubst %,$(BUILD)/%.o,$(basename $(LRVNE_OPUS_SRCS))): CFLAGS = $(LRVNE_OPUS_CFLAGS)
# <opus.h> for the decoder's user; with the definitions, as these reach every compile
LRVNE_DEFS       += -I$(LRVNE_OPUS_DIR)/include

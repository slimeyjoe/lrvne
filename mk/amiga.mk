# SPDX-License-Identifier: GPL-3.0-or-later
# What the targets of the Amiga family share (mk/target/amiga.mk, warpos.mk, aros.mk include
# this file): the switches their toolchains decide and the release archive rules.
# backends/amiga/README.md describes the backend.

# no C++ compiler in these toolchains: the light C core is the FM chip
override LRVNE_YMFM    := 0
# the test automation protocol needs a driver on stdin and stdout; tests of these targets go
# through AUTOPLAY, FAST, PRESENTTEST and the snapshot handshake (backends/amiga/core/snap.c)
override LRVNE_CONTROL := 0
# backends/amiga/core/paths.c is the AmigaDOS counterpart of the POSIX file
LRVNE_BACKEND_SKIP := $(LRVNE)/backends/common/paths.c
# release archives (after the rules of mk/lrvne.mk: they use $(EXE) and $(BUILD))
LRVNE_TARGET_POST  := $(LRVNE)/mk/amiga-dist.mk

# SPDX-License-Identifier: GPL-3.0-or-later
# The WinUAE rig's guest helpers (../README.md): uaequit, which every run's startup-sequence ends
# with, and waitwb, which waits for the Workbench in a display mode. vbcc's vc on PATH, as for
# the Amiga targets. Plain GNU make.

VC	?= vc +aos68k
CFLAGS	= -c99 -cpu=68020 -O1

all: uaequit waitwb

uaequit: uaequit.c
	$(VC) $(CFLAGS) -o $@ $< -lamiga

waitwb: waitwb.c
	$(VC) $(CFLAGS) -o $@ $< -lamiga

clean:
	rm -f uaequit waitwb

.PHONY: all clean

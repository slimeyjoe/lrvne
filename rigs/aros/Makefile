# SPDX-License-Identifier: GPL-3.0-or-later
# The AROS rig's guest helper (../README.md): dbgtype, which carries a QEMU run's output out
# through the kernel debug channel. x86_64-aros-gcc on PATH (AROS_SDK = the SDK sysroot), as
# for TARGET=aros. Plain GNU make.
CC	= x86_64-aros-gcc
CFLAGS	= -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter $(if $(AROS_SDK),--sysroot=$(AROS_SDK))

all: dbgtype

dbgtype: dbgtype.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f dbgtype

.PHONY: all clean

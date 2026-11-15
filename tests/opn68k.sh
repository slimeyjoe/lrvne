#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The 68k assembly render loop of the light OPN core against its C reference, sample for
# sample, on an emulated 68040 (tests/opn68k/opn68k.c says what is compared).
#
#   tests/opn68k.sh [BUILD-DIR]         (default build/opn68k; `make opn68k`)
#
# Needs vasm for the 68k with the Motorola syntax (the assembler the Amiga target uses), a
# GNU cross compiler for m68k Linux and qemu's user-mode emulator; no C library for the 68k.
# Other names than the defaults below go in VASM, M68K_CC and QEMU_M68K. Exits with 77 when a
# tool is missing, which says "skipped" rather than "failed".
#
# The Amiga build itself proves the structure offsets the assembly hard-codes for vbcc at
# compile time (src/snd/opn_lite.c). Here the same check runs for the GNU compiler's layout,
# which is the same 2-byte alignment, and then the code itself is run.
set -eu

out=${1:-build/opn68k}
VASM=${VASM:-vasmm68k_mot}
M68K_CC=${M68K_CC:-m68k-linux-gnu-gcc}
QEMU_M68K=${QEMU_M68K:-qemu-m68k}
here=$(cd "$(dirname "$0")/.." && pwd)

for tool in "$VASM" "$M68K_CC" "$QEMU_M68K"; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "opn68k: $tool not found, skipped" >&2
        exit 77
    }
done

mkdir -p "$out"
cflags="-m68040 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns
        -I$here/tests/opn68k/include -I$here/include"

# the core with the assembly loop; vbcc's __reg() means nothing to this compiler (the
# trampoline in opn68k.c stands in for it)
$M68K_CC $cflags -DLRV_OPN_ASM_68K '-D__reg(r)=' -c -o "$out/opn_asm.o" "$here/src/snd/opn_lite.c"
# the reference: the C loop, its external names out of the way
$M68K_CC $cflags -Dlrv_opn_lite_ops=ref_opn_lite_ops -Dlrv_opn_lite_pow=ref_opn_lite_pow \
    -Dlrv_opn_lite_wave=ref_opn_lite_wave -Dlrv_opn_lite_eg_due=ref_opn_lite_eg_due \
    -Dlrv_opn_lite_ssg_output=ref_opn_lite_ssg_output -Dlrv_opn_select=ref_opn_select -Dlrv_opn_flags=ref_opn_flags \
    -c -o "$out/opn_ref.o" "$here/src/snd/opn_lite.c"
$M68K_CC $cflags -c -o "$out/opn68k.o" "$here/tests/opn68k/opn68k.c"
$VASM -Felf -quiet -o "$out/opn_lite_68k.o" "$here/src/snd/opn_lite_68k.s"

# the assembly's names carry the underscore of the Amiga's object format
$M68K_CC -m68040 -nostdlib -static -o "$out/opn68k" \
    "$out/opn68k.o" "$out/opn_asm.o" "$out/opn_ref.o" "$out/opn_lite_68k.o" \
    -Wl,--defsym=_lrv_opn_lite_wave=lrv_opn_lite_wave \
    -Wl,--defsym=_lrv_opn_lite_pow=lrv_opn_lite_pow \
    -Wl,--defsym=_lrv_opn_lite_eg_due=lrv_opn_lite_eg_due \
    -Wl,--defsym=_lrv_opn_lite_ssg_output=lrv_opn_lite_ssg_output \
    -Wl,-z,noexecstack -lgcc

exec "$QEMU_M68K" -cpu m68040 "$out/opn68k"

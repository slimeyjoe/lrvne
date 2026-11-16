#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs PowerPC (WarpOS) programs in WinUAE with Blizzard PPC emulation (qemu-uae).
#
#   run-wos.sh NAME [wb] < commands
#
# NAME: work dir $RIG_WORKTOP/wos-NAME.  "wb" copies the Workbench tree (needed for
# screens/fonts/AHI); without it the guest is a bare shell.  The commands on stdin become
# the startup-sequence (see emu_script in ../amiga/run-lib.sh: plain lines append their
# output to DH0:out.txt, "!" lines run verbatim).  Files to run go into $W/dh0 before:
# set WOS_FILES="path..." (copied to DH0:). The game's snapshot requests (SNAPDIR, default
# its PROGDIR: = DH0:; an AUTOPLAY spec's snap: ticks, or PRESENTTEST) are answered by
# run-lib.sh's snapshot server (results in $W/snaps/; ../present.py checks a PRESENTTEST run).
#
# WOS_WB=MODEID:DEPTH starts a Workbench in that mode before the commands (needs "wb"; for
# the game's WINDOW option), WOS_RTG=1 adds a uaegfx Zorro III board (its modes for
# WOS_WB or MODE=).
#
# One run at a time: several qemu-uae instances starve each other on a 4-core host, so each
# run holds $WORKTOP/.ppc.lock and a second one waits for it. Real time (warp off): WarpOS
# needs the 68k and the PPC in step. Exits with the emulator's status (124/137: EMU_TIMEOUT).
#
# Settings (../rig.sh: the environment or the rig configuration): EMU, WINUAE_PPC (a WinUAE
# built with qemu-uae's PPC, default $EMU/obj-winuae-ppc/winuae), WARPUP (the WarpUP 5.1 tree),
# BPPC_ROM (the Blizzard PPC ROM),
# KICKSTART, WBDISK (for "wb"); GDB_PORT (default 2361, so it can run next to the 68k rig),
# EMU_TIMEOUT (default 1800), PPC_MODEL (603ev or 604e), WOS_CPU (68060 or 68040: the board's 68k; the 060
# ROM is the default). WOS_UAE="key=value ..." replaces or adds WinUAE config lines
# (experiments). RTAREA = where WinUAE put its boot ROM, which uaequit calls (default db0000:
# the address emu.log prints as "RTAREA located at").
set -u
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/../rig.sh"
EMU=${EMU:-$HOME/amiga-emu}
WINUAE=${WINUAE_PPC:-$EMU/obj-winuae-ppc/winuae}
GDB_PORT=${GDB_PORT:-2361}
WARPUP=${WARPUP:-}
[ -n "$WARPUP" ] || rig_need WARPUP "the WarpUP 5.1 tree"
PPC_MODEL=${PPC_MODEL:-603ev}
EMU_TIMEOUT=${EMU_TIMEOUT:-1800}
BPPC_ROM=${BPPC_ROM:-$EMU/rom/blizzardppc_060_44.71.rom}
. "$RIG/run-lib.sh"

name=$1
emu_setup "wos-$name" "${2:-}"

# WarpOS 5.1 runtime: kernel, warp.library, the Blizzard PPC hardware driver
cp "$WARPUP/Libs/powerpc.library" "$WARPUP/Libs/warp.library" \
    "$WARPUP/hwdrivers/blizzardppc/warpHW.library" "$W/dh0/Libs/"
for f in ${WOS_FILES:-}; do
	cp "$f" "$W/dh0/"
done

# ENV: for WarpOS: the Blizzard PPC flash ROM starts Phase5's ppc.library at boot and it
# can't be switched off, so WarpOS needs its "terminator" (powerpc.guide: always 2);
# HideWarning skips the ppc.library requester that would block the unattended run
mkdir -p "$W/dh0/Env/powerpc"
echo 2 > "$W/dh0/Env/powerpc/Terminator"
echo 1 > "$W/dh0/Env/powerpc/HideWarning"
[ -n "${WOS_WB:-}" ] && emu_wbmode "${WOS_WB%%:*}" "${WOS_WB#*:}"
{
	echo "!Assign ENV: DH0:Env"
	if [ -n "${WOS_WB:-}" ]; then
		echo "!C:MakeDir RAM:T RAM:Clipboards"
		echo "!Assign T: RAM:T"
		echo "!Assign CLIPS: RAM:Clipboards"
		echo "!C:Copy >NIL: ENVARC: ENV: ALL NOREQ"
		echo "!Assign LIBS: SYS:Classes ADD"
		echo "!DEVS:Monitors/PAL"
		[ "${WOS_RTG:-0}" = 1 ] && echo "!DEVS:Monitors/uaegfx"
		echo "!C:IPrefs"
		echo "!C:LoadWB"
		echo "C:waitwb ${WOS_WB%%:*}"        # the Workbench in its mode: an event
	fi
	cat
} | emu_script
# WinUAE moves its boot ROM (the uaelib trap uaequit calls) when the board ROM takes
# 0xf00000; RTAREA is the address the emulator log prints ("RTAREA located at")
sed -i "s|^C:uaequit\$|C:uaequit ${RTAREA:-db0000}|" "$W/dh0/S/startup-sequence"

# Blizzard PPC: A1200, the board's 68060 + a PPC 603e/604e through qemu-uae, 128 MB on the
# board.  Real time (warp off): WarpOS boots with the 68k and the PPC in step.
cat > "$W/a.uae" <<EOF
config_description=lrvne rig, WarpOS ($name)
use_gui=no
kickstart_rom_file=$KICKSTART
chipset=aga
chipset_compatible=A1200
cpu_model=68060
mmu_model=68060
fpu_model=68060
cpu_compatible=false
cpu_24bit_addressing=false
cpu_speed=max
cachesize=0
cycle_exact=false
immediate_blits=true
warp=false
cpuboard_type=BlizzardPPC
cpuboard_rom_file=$BPPC_ROM
cpuboardmem1_size=128
ppc_model=$PPC_MODEL
ppc_implementation=qemu
chipmem_size=4
fastmem_size=0
nr_floppies=1
floppy0type=0
filesystem2=rw,DH0:dh0:$W/dh0,10
debugging_features=gdbserver
gdb_port=$GDB_PORT
EOF

# extra config lines (experiments): WOS_UAE="key=value key=value"
for kv in ${WOS_UAE:-}; do
	k=${kv%%=*}
	sed -i "/^$k=/d" "$W/a.uae"
	echo "$kv" >> "$W/a.uae"
done

if [ "${WOS_RTG:-0}" = 1 ]; then
	cat >> "$W/a.uae" <<EOF
z3mem_size=16
gfxcard_type=ZorroIII
gfxcard_size=16
EOF
fi

# one qemu-uae at a time (the header)
exec 9>"$WORKTOP/.ppc.lock"
if ! flock -n 9; then
	echo "== another WarpOS run is going on: waiting for it to end" >&2
	flock 9
fi
emu_run
exit "$EMU_RC"

#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs an Amiga (68k) program of an engine on lrvne in WinUAE (no video output), with the rig's
# snapshot server answering its snapshot requests through the gdbserver (../README.md).
#
#   run-program.sh PROGRAM [native|rtg|promote] [MHz]
#
#   native   AGA screen, no RTG (68040, cpu_speed=max unless MHz is given -> cycle exact)
#   rtg      Workbench + Picasso96 loaded, the program on the native mode (BltBitMap patched)
#   promote  FAKENATIVEMODES: HiRes Laced promoted to a uaegfx screen
#
# PROGRAM (the executable) is copied to DH0:<DRAWER>/ (DRAWER default: its file name) and run
# there, from a Workbench tree, with PROGRAM_ARGS (PROGRAM_ARGS2: a second run in the same
# session, to compare `Avail`): the pack and anything else it needs go in with GUEST_FILES
# (copied into the drawer; name the pack with PAK=PROGDIR:...), the saves it starts with with
# GUEST_SAVES=dir (its files copied to DH0:<DRAWER>/<SAVES_DIR>/, SAVES_DIR default saves).
# (An AUTOPLAY value in PROGRAM_ARGS must be quoted: ReadArgs treats an unquoted '=' as a
# delimiter.) Snapshots (an autoplay spec's snap: ticks, PRESENTTEST) land in <work dir>/snaps/.
#
# Real time by default: a run in real time measures timing (the frame clock, the audio
# interrupts: the statistics at the end of out.txt are measurements, not a pass/fail check).
# WARP=1 runs unthrottled, which is what a conformance check wants: FAST, PRESENTTEST and
# autoplay count ticks, so the results are the same and a busy host only makes a run longer.
#
# AHI=1 installs AHI 6 (ahi.device, paula.audio, Paula modes, prefs: unit 0 = 8-bit stereo
# 22050 Hz) from $EMU/aos/ahi/ into the guest. Audio interrupts exist only in a WinUAE built
# with SDL3 audio: WINUAE=<that binary> (SDL_AUDIO_DRIVER=dummy is set, no sound device is
# needed). WB=MODEID:DEPTH starts a Workbench in that mode first (for WINDOW: the program in a
# window on it; run-lib.sh emu_wbmode), waited for by waitwb. PRE_CMD is a startup-sequence
# line run just before the program. RUN=name and GDB_PORT=n let several runs go in parallel
# (default work dir run-<program>-<variant>, port 2345; runs on one port wait for each other).
# Output: work dir $RIG_WORKTOP/<name>/ with out.txt (the program's console log), emu.rc (the
# emulator's exit status, 124/137: EMU_TIMEOUT, default 300 s, ended it) and snaps/ (the
# snapshot server's screenshots, bitmaps and manifest.jsonl). Exits with the emulator's status.
set -eu
PROGRAM=${1:?usage: run-program.sh PROGRAM [native|rtg|promote] [MHz]}
variant=${2:-native}
mhz=${3:-}
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/run-lib.sh"
[ -f "$PROGRAM" ] || { echo "run-program.sh: no program $PROGRAM" >&2; exit 2; }
prog=$(basename "$PROGRAM")
EMU_DRAWER=${DRAWER:-$prog}
PROGRAM_ARGS=${PROGRAM_ARGS:-}

emu_setup "${RUN:-run-$prog-$variant}" wb
D=$W/dh0/$EMU_DRAWER
mkdir -p "$D"
cp "$PROGRAM" "$D/$prog"
chmod 755 "$D/$prog"
for f in ${GUEST_FILES:-}; do
	cp "$f" "$D/"
done
if [ -n "${GUEST_SAVES:-}" ]; then
	mkdir -p "$D/${SAVES_DIR:-saves}"
	cp "$GUEST_SAVES"/* "$D/${SAVES_DIR:-saves}/" 2>/dev/null || true
fi
if [ "${AHI:-0}" = 1 ]; then
	AHIDIR=$EMU/aos/ahi
	mkdir -p "$W/dh0/Devs/AHI" "$W/dh0/Devs/AudioModes" "$W/dh0/Prefs/Env-Archive/Sys"
	cp "$AHIDIR/Devs/ahi.device" "$W/dh0/Devs/"
	cp "$AHIDIR/Devs/AHI/paula.audio" "$W/dh0/Devs/AHI/"
	cp "$AHIDIR/Devs/AudioModes/PAULA" "$W/dh0/Devs/AudioModes/"
	python3 "$HERE/mkahiprefs.py" "$W/dh0/Prefs/Env-Archive/Sys/ahi.prefs"
fi
export SDL_AUDIO_DRIVER=dummy
if [ -n "${WB:-}" ]; then
	emu_wbmode "${WB%%:*}" "${WB#*:}"
fi

case $variant in
native)
	monitors=""
	;;
promote)
	python3 "$HERE/settooltype.py" "$WBDISK/Devs/Monitors/uaegfx.info" \
	    "$W/dh0/Devs/Monitors/uaegfx.info" 'FAKENATIVEMODES=Yes'
	monitors="DEVS:Monitors/uaegfx"
	;;
rtg)
	monitors="DEVS:Monitors/PAL
DEVS:Monitors/uaegfx"
	;;
*)
	echo "unknown variant $variant" >&2
	exit 2
	;;
esac

{
	cat <<EOF
!C:SetPatch QUIET
!C:MakeDir RAM:T RAM:ENV RAM:ENV/Sys RAM:Clipboards
!C:Copy >NIL: ENVARC: RAM:ENV ALL NOREQ
!Assign ENV: RAM:ENV
!Assign T: RAM:T
!Assign CLIPS: RAM:Clipboards
!Assign LIBS: SYS:Classes ADD
EOF
	if [ -n "${WB:-}" ] && [ -z "$monitors" ]; then
		monitors="DEVS:Monitors/PAL"
	fi
	if [ -n "$monitors" ]; then
		echo "$monitors" | sed 's/^/!/'
		echo "!C:IPrefs"
	fi
	if [ -n "${WB:-}" ]; then
		echo "!C:LoadWB"
		# the Workbench in the mode of its prefs (emu_wbmode): an event, not a few seconds
		echo "C:waitwb ${WB%%:*}"
	fi
	echo "Version"
	echo "Avail"
	# PRE_CMD (see the header)
	[ -n "${PRE_CMD:-}" ] && echo "!$PRE_CMD"
	echo "DH0:$EMU_DRAWER/$prog $PROGRAM_ARGS"
	echo "Avail"
	if [ -n "${PROGRAM_ARGS2:-}" ]; then
		# a second run in the same session shows whether memory is returned
		echo "DH0:$EMU_DRAWER/$prog $PROGRAM_ARGS2"
		echo "Avail"
	fi
} | emu_script

if [ "$variant" = native ]; then
	emu_config native
else
	emu_config rtg
fi
# 68040 in real time (the frame clock and the audio interrupts are what we test)
sed -i -e "s/^cpu_model=.*/cpu_model=68040/" -e "s/^warp=.*/warp=false/" "$W/a.uae"
cat >> "$W/a.uae" <<EOF3
fpu_model=68040
mmu_model=0
sound_output=interrupts
EOF3
if [ -n "$mhz" ]; then
	sed -i -e "s/^cpu_compatible=.*/cpu_compatible=true/" -e "s/^cpu_speed=.*/cpu_speed=real/" "$W/a.uae"
	cat >> "$W/a.uae" <<EOF4
cpu_cycle_exact=true
cpu_memory_cycle_exact=true
cpu_frequency=${mhz}000000
cpu_data_cache=true
EOF4
fi

if [ "${WARP:-0}" = 1 ]; then
	sed -i -e "s/^warp=.*/warp=true/" "$W/a.uae"
fi

emu_run
grep -i -E 'Unix RTG|uaegfx|P96|exception|guru|illegal' "$W/emu.log" | head -20 || true
exit "$EMU_RC"

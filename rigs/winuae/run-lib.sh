# SPDX-License-Identifier: GPL-3.0-or-later
# The WinUAE plumbing of the rigs (run-program.sh, run-wos.sh, check-dist.sh) and of an engine's
# own run scripts. Sourced, not run, after the caller set RIG to this directory (where uaequit
# and waitwb are built: make -C "$RIG"); HERE is the caller's own directory.
#
# Nothing here waits on the clock (../README.md "Events, not time"): the guest's requests are
# answered by ../snapd.py through WinUAE's gdbserver, which stops the CPU; the run ends when the
# startup-sequence quits the emulator; EMU_TIMEOUT (wall clock, `timeout`) is the one backstop,
# for a run that is stuck. A guest watchdog counting emulated seconds is not one: with
# cpu_speed=max the CPU's share of a frame is measured in host time, so emulated time itself
# depends on how busy the host is.
#
# Settings (../rig.sh: the environment or the rig configuration): EMU (default ~/amiga-emu),
# WINUAE (a WinUAE for Linux with the gdbserver, null video), WBDISK (read-only Workbench 3.1 +
# Picasso96 tree), KICKSTART, GDB_PORT, RIG_WORKTOP (the work directories, default
# $EMU/aos/work/rig). EMU_C_FILES="path..." are copied into the guest's C: as well (a probe's
# programs); EMU_DRAWER names the program's drawer in DH0:, whose snapshot requests are served
# too. A run lives in $RIG_WORKTOP/<name>/: dh0/ (the guest's DH0:), a.uae, emu.log, emu.rc,
# snaps/ and shots/, out.txt (copied from dh0/out.txt).

. "$RIG/../rig.sh"
EMU=${EMU:-$HOME/amiga-emu}
WINUAE=${WINUAE:-$EMU/obj-winuae/winuae}
WBDISK=${WBDISK:-}
KICKSTART=${KICKSTART:-$EMU/rom/kick40068.A1200}
GDB_PORT=${GDB_PORT:-2345}
EMU_TIMEOUT=${EMU_TIMEOUT:-300}
HERE=$(cd "$(dirname "$0")" && pwd)
WORKTOP=${RIG_WORKTOP:-$EMU/aos/work/rig}

# emu_setup NAME [wb]: fresh work dir; "wb" copies the Workbench/P96 tree.
emu_setup()
{
	W=$WORKTOP/$1
	rm -rf "$W"
	mkdir -p "$W/dh0/C" "$W/dh0/S" "$W/dh0/T" "$W/dh0/Libs" "$W/shots"
	if [ "${2:-}" = wb ]; then
		rig_need WBDISK "a Workbench 3.1 + Picasso96 tree"
		# Selective copy: leave out dev kits, docs and games.
		for d in C Libs Devs S L Prefs Classes Locale Fonts System \
		    Utilities Tools WBStartup Rexxc Expansion Storage; do
			[ -d "$WBDISK/$d" ] && cp -R "$WBDISK/$d" "$W/dh0/"
		done
		cp "$WBDISK"/*.info "$W/dh0/" 2>/dev/null || true
		chmod -R u+w "$W/dh0"
	fi
	cp "$EMU"/aos/C/* "$W/dh0/C/"
	# Run and Execute (from the Workbench tree), for a "Run >NIL: ..." in a startup line
	for c in Run Execute; do
		[ -n "$WBDISK" ] || break
		[ -e "$W/dh0/C/$c" ] || cp "$WBDISK/C/$c" "$W/dh0/C/" 2>/dev/null || true
	done
	for t in uaequit waitwb; do
		[ -x "$RIG/$t" ] || { echo "run-lib.sh: build $RIG/$t first (make -C $RIG)" >&2; exit 2; }
	done
	cp "$RIG/uaequit" "$RIG/waitwb" ${EMU_C_FILES:-} "$W/dh0/C/"
	chmod 755 "$W"/dh0/C/*
}

# emu_wbmode MODEID DEPTH: the Workbench of the run opens in MODEID (hex) with DEPTH planes
# (the mode's default size): screenmode.prefs into the tree's own Sys/ drawer (a second "sys/"
# would be copied over it); host mouse integration removed (probes inject their own input).
emu_wbmode()
{
	envsys="$W/dh0/Prefs/Env-Archive/Sys"
	[ -d "$envsys" ] || envsys="$W/dh0/Prefs/Env-Archive/sys"
	mkdir -p "$envsys"
	rm -f "$W/dh0/WBStartup/mousehack" "$W/dh0/WBStartup/timehack"
	python3 - "$envsys/screenmode.prefs" "$1" "$2" <<'EOF'
import struct, sys
def chunk(cid, body):
    return cid + struct.pack('>I', len(body)) + body + (b'\0' if len(body) & 1 else b'')
mode, depth = int(sys.argv[2], 16), int(sys.argv[3])
# width/height ~0 = the mode's default ("Default" in ScreenMode prefs)
body = b'PREF' + chunk(b'PRHD', struct.pack('>BBI', 0, 0, 0)) \
    + chunk(b'SCRM', struct.pack('>4IIHHHH', 0, 0, 0, 0, mode, 0xFFFF, 0xFFFF, depth, 1))
open(sys.argv[1], 'wb').write(chunk(b'FORM', body))
EOF
}

# emu_cleanup: stops whatever emu_run started (trap handler): TERM, then KILL for what is
# still there once TERM was dealt with (the emulator's group: `timeout` and WinUAE).
emu_cleanup()
{
	if [ -n "${EMU_PID:-}" ] && kill -0 "$EMU_PID" 2>/dev/null; then
		echo "== stopping emulator $EMU_PID" >&2
		kill "$EMU_PID" 2>/dev/null
		wait "$EMU_PID" 2>/dev/null
	fi
	pkill -9 -f "winuae -f $W/a.uae" 2>/dev/null || true
	if [ -n "${EMU_SNAPD_PID:-}" ]; then
		kill "$EMU_SNAPD_PID" 2>/dev/null || true
	fi
	if [ -n "${EMU_HELPER_PID:-}" ]; then
		kill "$EMU_HELPER_PID" 2>/dev/null || true
	fi
}

# emu_script < lines: writes S/startup-sequence.  Lines starting with "!"
# run verbatim; others run with stdout appended to DH0:out.txt.  The last line
# quits the emulator; a run that never gets there ends at EMU_TIMEOUT.
emu_script()
{
	{
		echo "FailAt 21"
		while IFS= read -r line; do
			case "$line" in
			"!"*)
				echo "${line#!}"
				;;
			*)
				# AmigaDOS escapes a quote inside a quoted string as *"
				esc=$(printf '%s' "$line" | sed 's/"/*"/g')
				printf 'Echo "%% %s" >>DH0:out.txt\n' "$esc"
				echo "$line >>DH0:out.txt"
				;;
			esac
		done
		echo "C:uaequit"
	} > "$W/dh0/S/startup-sequence"
}

# emu_config native|rtg: writes a.uae.
emu_config()
{
	{
		cat <<EOF
config_description=lrvne rig ($1)
use_gui=no
kickstart_rom_file=$KICKSTART
chipset=aga
chipset_compatible=A1200
cpu_model=68020
cpu_compatible=false
cpu_24bit_addressing=false
cpu_speed=max
cachesize=0
cycle_exact=false
immediate_blits=true
warp=true
chipmem_size=4
fastmem_size=8
nr_floppies=1
floppy0type=0
filesystem2=rw,DH0:dh0:$W/dh0,10
debugging_features=gdbserver
gdb_port=$GDB_PORT
EOF
		if [ "$1" = rtg ]; then
			cat <<EOF
z3mem_size=16
gfxcard_type=ZorroIII
gfxcard_size=16
EOF
		fi
	} > "$W/a.uae"
}

# emu_run: runs the emulator, bounded by EMU_TIMEOUT, with tests/e2e/amiga_snapd.py answering
# the guest's requests in DH0: and DH0:$EMU_DRAWER/ (snap_*.req -> snaps/, the probes' shot_*.req
# -> shots/) until it exits.
#
# One run per gdb port at a time: the port's lock ($WORKTOP/.port-<port>.lock) is held for
# the run, so a second run on the same port waits for the first instead of answering its
# guest from the wrong emulator (WinUAE only logs a port it cannot listen on). The run starts
# serving once emu.log says the gdbserver listens, and fails if it says it cannot.
#
# The outcome: EMU_RC and $W/emu.rc (written first) hold the emulator's exit status, 124 or
# 137 when the backstop ended it. emu_run itself returns 0, so callers under `set -e` still
# print what the run left; they decide by EMU_RC.
emu_run()
{
	local pid start rc port_lock
	: > "$W/dh0/out.txt"
	rm -f "$W/emu.rc"
	trap 'emu_cleanup' EXIT INT TERM HUP
	mkdir -p "$WORKTOP"
	port_lock=$WORKTOP/.port-$GDB_PORT.lock
	exec 8>"$port_lock"
	if ! flock -n 8; then
		echo "== gdb port $GDB_PORT is in use by another run: waiting for it to end" >&2
		flock 8
	fi
	start=$(date +%s)
	timeout -k 10 "$EMU_TIMEOUT" "$WINUAE" -f "$W/a.uae" > "$W/emu.log" 2>&1 &
	pid=$!
	EMU_PID=$pid
	# the gdbserver listens before the guest boots: wait for the log to say so (or not)
	while kill -0 "$pid" 2>/dev/null; do
		grep -q "WinUAE GDB: listening on 127.0.0.1:$GDB_PORT\$" "$W/emu.log" 2>/dev/null && break
		if grep -q "WinUAE GDB: cannot listen on 127.0.0.1:$GDB_PORT\$" "$W/emu.log" 2>/dev/null; then
			echo "== $W: the gdb port $GDB_PORT is taken" >&2
			kill "$pid" 2>/dev/null
			break
		fi
		sleep 0.1
	done
	python3 -I "$RIG/../snapd.py" --port "$GDB_PORT" --dir "$W/dh0" \
	    ${EMU_DRAWER:+--dir "$W/dh0/$EMU_DRAWER"} --out "$W/snaps" --shots "$W/shots" --pid "$pid" \
	    > "$W/snapd.log" 2>&1 &
	EMU_SNAPD_PID=$!
	rc=0
	wait "$pid" || rc=$?
	EMU_PID=
	echo "$rc" > "$W/emu.rc"
	EMU_RC=$rc
	if ! kill -0 "$EMU_SNAPD_PID" 2>/dev/null; then
		wait "$EMU_SNAPD_PID" 2>/dev/null || echo "== $W: the snapshot server ended early (snapd.log)" >&2
	fi
	kill "$EMU_SNAPD_PID" 2>/dev/null || true
	EMU_SNAPD_PID=
	flock -u 8
	exec 8>&-
	cp "$W/dh0/out.txt" "$W/out.txt"
	case $rc in
	124|137) echo "== $W: timeout (backstop) after $(( $(date +%s) - start )) s: EMU_TIMEOUT=$EMU_TIMEOUT" ;;
	*)       echo "== $W: emulator exit $rc" ;;
	esac
	cat "$W/out.txt"
	ls "$W/shots" 2>/dev/null
	return 0
}

#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs AmigaDOS commands in AROS (x86_64, ABIv11): hosted, or the native live CD in QEMU/KVM.
# The AROS rig of rigs/README.md.
#
#   aros.sh hosted RUNDIR [SECONDS] < commands
#   aros.sh qemu RUNDIR [SECONDS] < commands
#
# Settings (rigs/rig.sh: the environment or the rig.conf): AROS_HOSTED, the unpacked hosted
# system (AROS-202609-linux-x86_64-system), and for qemu AROS_ISO (the pc-x86_64 boot ISO);
# both default below AROS_DIR, the directory with the unpacked releases (hosted/, native/).
#
# hosted: the system is copied to RUNDIR/AROS (AROS writes into SYS:, so no two runs share a
# tree); the files in AROS_FILES="path ..." go to SYS:Work/, assigned Work:, the current
# directory. The commands on stdin become S:User-Startup, run before Wanderer starts:
#   plain lines   run with their output appended to SYS:out.txt (RUNDIR/out.txt at the end)
#   "!" lines     go in verbatim (without the "!")
# The last line writes a marker; the host waits for it, or for AROS to end, and stops AROS
# (KEEP=1: no marker, for an interactive session with VISIBLE=1, which lasts until AROS is
# closed). SECONDS (default 120) is the backstop, for a run that is stuck, never the end of a
# normal one. Nothing here waits for a number of seconds.
#
# Exit status: 0 the commands finished; 3 AROS ended before they did; 4 the X server could not
# start; 124 the backstop ended the run. RUNDIR/run.log says which.
#
# Options (environment): VISIBLE=1 uses the desktop's X ($DISPLAY) instead of a headless one
# (xwfb-run: Xwayland on a headless weston); NODISPLAY=1 runs without X (AROS then starts an
# emergency shell on the host's stdin and the startup sequence does not run: manual use, the
# run lasts SECONDS); SHOT_END=1 takes a screenshot when the commands are done
# (RUNDIR/shots/end.png); PRUNE=1 removes the system tree afterwards and keeps SYS:Work/ as
# RUNDIR/Work, without the packs (*.pak) copied into it. Sound: hosted AHI plays through ALSA;
# the run's HOME is RUNDIR, whose .asoundrc sends ALSA to a private PipeWire null sink that
# exists only for the run (real-time pacing, nothing audible), recorded to RUNDIR/audio.wav
# (AUDIO_OUT=host: the host's speakers; without pactl: no sink, run programs with AUDIO=none).
# An ALSA null or file device does not do: it has no clock, and AHI hangs. AROS runs with a
# PipeWire client setting without data-loop threads (README.md, "Hosted AROS and PipeWire": its
# ALSA driver lets PipeWire 1.6 start that thread with AROS's signals unblocked, and AROS then
# crashes now and then); run.log notes it; AROS_PW_DATA_LOOPS=1 keeps PipeWire's default.
#
# qemu: the files and a User-Startup are added to a copy of the ISO (xorriso; the live CD is
# SYS:, read-only), so the commands' output goes to RAM:out.txt and leaves through dbgtype
# (this directory's Makefile builds it) on the kernel debug output, which is the serial port
# with debug=serial (KERNEL_ARGS= to change): RUNDIR/serial.log, out.txt cut from it. VGA=vmware
# (default) or std; sound from an ES1370 to audio.wav; SHOT_END by the monitor's screendump;
# QEMU= the binary (default qemu-system-x86_64), QEMU_ARGS= more arguments. The run ends at
# the end marker on the serial log or when QEMU ends; SECONDS is the backstop.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../rig.sh"
AROS_DIR=${AROS_DIR:-}
die() { echo "aros.sh: $*" >&2; exit 2; }

# stop_group PID: TERM to the process group PID leads, wait for the leader, KILL what is left
stop_group() {
    kill -- -"$1" 2>/dev/null
    wait "$1" 2>/dev/null
    kill -9 -- -"$1" 2>/dev/null
}

# _inner RUNDIR: inside the X session (or without X): boot, wait for the end, stop. The end is
# an event: the marker the last command writes, or AROS ending; the caller's `timeout` is the
# backstop. Exit status 0 / 3 as aros.sh's.
inner() {
    local R=$1 rc
    cd "$R/AROS" || exit 1
    # its own process group: AROSBootstrap forks, and the whole group goes at the end (a
    # global for the trap: a local would be out of scope when the EXIT trap runs)
    ${AROS_XDG:+env XDG_CONFIG_HOME="$AROS_XDG"} setsid boot/linux/AROSBootstrap >"$R/boot.log" 2>&1 &
    AROS_PID=$!
    echo $AROS_PID >"$R/aros.pgid"     # for the caller, should the backstop kill this shell
    trap 'stop_group $AROS_PID' EXIT INT TERM
    while [ ! -e "$R/AROS/done.marker" ] && kill -0 $AROS_PID 2>/dev/null; do
        sleep 0.1
    done
    if [ -e "$R/AROS/done.marker" ]; then
        echo "aros.sh: done after ${SECONDS}s" >>"$R/run.log"
        rc=0
    elif [ "${KEEP:-0}" = 1 ]; then
        echo "aros.sh: AROS was closed after ${SECONDS}s" >>"$R/run.log"
        rc=0
    else
        echo "aros.sh: AROS ended after ${SECONDS}s before the commands were done" >>"$R/run.log"
        rc=3
    fi
    [ "${SHOT_END:-0}" = 1 ] && [ -n "${DISPLAY:-}" ] && import -window root "$R/shots/end.png" 2>>"$R/run.log"
    return $rc
}

# outcome RC: the exit status of the run (inner's, or the backstop's), logged once. After the
# backstop AROS's process group may outlive the X session that `timeout` killed: it goes too.
outcome() {
    [ -s "$R/aros.pgid" ] && kill -9 -- -"$(cat "$R/aros.pgid")" 2>/dev/null
    case $1 in
    0|3) return $1 ;;
    124|137) echo "aros.sh: timeout (the backstop: ${secs}s), the run did not end" >>"$R/run.log"; return 124 ;;
    255) echo "aros.sh: the X server could not start (xwfb-run)" >>"$R/run.log"; return 4 ;;
    *) echo "aros.sh: exit status $1" >>"$R/run.log"; return "$1" ;;
    esac
}

# rundir PATH: a fresh run directory, absolute
rundir() {
    mkdir -p "$1" || die "cannot make $1"
    R=$(cd "$1" && pwd)
    rm -rf "$R/AROS" "$R/Work" "$R/stage" "$R/shots" "$R/out.txt" "$R/run.log" "$R/boot.log" \
        "$R/serial.log" "$R/aros.pgid" "$R/audio.wav" "$R/xdg"
    mkdir -p "$R/shots"
}

case ${1:-} in
_inner)
    inner "$2"
    exit $?
    ;;
hosted)
    rundir "${2:?usage: aros.sh hosted RUNDIR [SECONDS] < commands}"
    secs=${3:-120}
    AROS_HOSTED=${AROS_HOSTED:-${AROS_DIR:+$AROS_DIR/hosted/AROS-202609-linux-x86_64-system}}
    [ -n "$AROS_HOSTED" ] || rig_need AROS_HOSTED "the unpacked hosted AROS system"
    [ -x "$AROS_HOSTED/boot/linux/AROSBootstrap" ] || die "no hosted AROS in $AROS_HOSTED"
    cp -a "$AROS_HOSTED" "$R/AROS" || die "cannot copy the hosted system"
    mkdir -p "$R/AROS/Work"
    for f in ${AROS_FILES:-}; do
        cp -a "$f" "$R/AROS/Work/" || die "cannot copy $f"
    done
    # S:User-Startup (replaces the copy's; the pristine tree is never touched)
    {
        echo 'Assign Work: SYS:Work'
        echo 'CD Work:'
        if [ "${NODISPLAY:-0}" != 1 ]; then
            while IFS= read -r line; do
                case $line in
                '!'*) printf '%s\n' "${line#!}" ;;
                '') ;;
                *) printf 'Echo "%% %s" >>SYS:out.txt\n%s >>SYS:out.txt\n' "${line//\"/*\"}" "$line" ;;
                esac
            done
        fi
        [ "${KEEP:-0}" = 1 ] || echo 'Echo done >SYS:done.marker'
    } >"$R/AROS/S/User-Startup"
    if [ "${AUDIO_OUT:-wav}" = wav ] && command -v pactl >/dev/null && command -v pw-record >/dev/null; then
        sink=lrvne_aros_$$
        mod=$(pactl load-module module-null-sink sink_name="$sink" \
                  sink_properties=device.description="$sink") || die "cannot create a null sink"
        printf 'pcm.!default {\n    type pipewire\n    playback_node "%s"\n}\n' "$sink" >"$R/.asoundrc"
        export HOME=$R
        pw-record --target "$sink" -P '{ stream.capture.sink=true }' "$R/audio.wav" \
            >"$R/record.log" 2>&1 &
        rec=$!
        trap 'kill $rec 2>/dev/null; wait $rec 2>/dev/null; pactl unload-module $mod' EXIT
    fi
    # AROS's ALSA driver (AHI's alsa.audio, AROS 2026.09) blocks the host's signals only around
    # opening the device; PipeWire 1.6 starts its data-loop thread later, in snd_pcm_hw_params,
    # so the thread takes AROS's timer signal now and then and runs AROS's interrupt code on its
    # own stack: an alert ("Stack extends out of range") or a trap. Without data loops the stream
    # runs on the plugin's thread, which starts inside the guarded call. For AROS only (not the X
    # server); AROS_PW_DATA_LOOPS=1 keeps PipeWire's default, to see the fault.
    if [ "${AROS_PW_DATA_LOOPS:-0}" != 1 ]; then
        mkdir -p "$R/xdg/pipewire/client.conf.d"
        printf 'context.properties = {\n    context.num-data-loops = 0\n}\n' \
            >"$R/xdg/pipewire/client.conf.d/lrvne-rig.conf"
        export AROS_XDG=$R/xdg
        echo "aros.sh: note: PipeWire without data-loop threads, for hosted AROS's ALSA driver" \
             "(rigs/README.md; AROS_PW_DATA_LOOPS=1 keeps them)" >>"$R/run.log"
    fi
    if [ "${NODISPLAY:-0}" = 1 ]; then
        ( cd "$R/AROS" && env -u DISPLAY -u WAYLAND_DISPLAY ${AROS_XDG:+XDG_CONFIG_HOME="$AROS_XDG"} \
              timeout -k 5 "$secs" boot/linux/AROSBootstrap ) 2>&1 | tee "$R/boot.log"
        rc=0
    elif [ "${VISIBLE:-0}" = 1 ]; then
        timeout -k 10 "$secs" "$0" _inner "$R"
        outcome $?
        rc=$?
    else
        command -v xwfb-run >/dev/null || die "xwfb-run is missing (xwayland-run)"
        timeout -k 10 "$secs" xwfb-run -- "$0" _inner "$R" >>"$R/run.log" 2>&1
        outcome $?
        rc=$?
    fi
    cp "$R/AROS/out.txt" "$R/out.txt" 2>/dev/null
    if [ "${PRUNE:-0}" = 1 ]; then
        find "$R/AROS/Work" -name '*.pak' -delete
        mv "$R/AROS/Work" "$R/Work"
        rm -rf "$R/AROS"
    fi
    cat "$R/run.log" 2>/dev/null
    echo "run dir: $R"
    exit $rc
    ;;
qemu)
    rundir "${2:?usage: aros.sh qemu RUNDIR [SECONDS] < commands}"
    secs=${3:-180}
    ISO=${AROS_ISO:-${AROS_DIR:+$AROS_DIR/native/AROS-202609-pc-x86_64-boot-iso/aros-pc-x86_64.iso}}
    [ -n "$ISO" ] || rig_need AROS_ISO "the AROS pc-x86_64 boot ISO"
    [ -f "$ISO" ] || die "no AROS ISO at $ISO"
    [ -x "$HERE/dbgtype" ] || die "build $HERE/dbgtype first (make -C $HERE)"
    mkdir -p "$R/stage/Work" "$R/stage/S" "$R/stage/grub"
    for f in ${AROS_FILES:-} "$HERE/dbgtype"; do
        cp -a "$f" "$R/stage/Work/" || die "cannot copy $f"
    done
    {
        echo 'Assign Work: SYS:Work'
        echo 'CD Work:'
        while IFS= read -r line; do
            case $line in
            '!'*) printf '%s\n' "${line#!}" ;;
            '') ;;
            *) printf 'Echo "%% %s" >>RAM:out.txt\n%s >>RAM:out.txt\n' "${line//\"/*\"}" "$line" ;;
            esac
        done
        echo 'Work:dbgtype ECHO RIG-OUT-BEGIN'
        echo 'Work:dbgtype RAM:out.txt'
        echo 'Work:dbgtype ECHO RIG-OUT-END'
    } >"$R/stage/S/User-Startup"
    # the boot menu without its 5 s wait; the copy boots by BIOS El Torito only (the original's
    # EFI/GPT hybrid does not replay: its GPT partitions overlap), with the GRUB image's boot
    # info patched for the new layout
    xorriso -indev "$ISO" -osirrox on -extract /boot/grub/grub.cfg "$R/stage/grub/grub.cfg" \
        >/dev/null 2>&1 || die "cannot read grub.cfg"
    chmod u+w "$R/stage/grub/grub.cfg"
    sed -i -e 's/^set timeout=.*/set timeout=0/' \
        -e "s|^\\( *multiboot2 /boot/pc/bootstrap.gz.*\\)\$|\\1 ${KERNEL_ARGS:-debug=serial}|" \
        "$R/stage/grub/grub.cfg"
    xorriso -indev "$ISO" -outdev "$R/test.iso" -map "$R/stage/Work" /Work \
        -map "$R/stage/S/User-Startup" /S/User-Startup \
        -map "$R/stage/grub/grub.cfg" /boot/grub/grub.cfg \
        -boot_image any discard -boot_image any cat_path=/boot.catalog \
        -boot_image grub bin_path=/boot/grub/i386-pc/eltorito.img \
        -boot_image grub grub2_boot_info=on -boot_image any boot_info_table=on \
        -boot_image any load_size=2048 \
        >"$R/xorriso.log" 2>&1 || die "xorriso failed (xorriso.log)"
    (
        trap 'kill $qp 2>/dev/null; wait $qp 2>/dev/null' EXIT INT TERM
        ${QEMU:-qemu-system-x86_64} -machine q35 -enable-kvm -cpu host -m 2048 -smp 2 \
            -cdrom "$R/test.iso" -boot d -vga "${VGA:-vmware}" -display none -no-reboot \
            -serial file:"$R/serial.log" -monitor unix:"$R/mon.sock",server,nowait \
            -audiodev wav,id=a0,path="$R/audio.wav" -device ES1370,audiodev=a0 \
            ${QEMU_ARGS:-} >"$R/qemu.log" 2>&1 &
        qp=$!
        # the end is an event: the end marker on the serial log, or QEMU ending; SECONDS is
        # the backstop
        end=$((SECONDS + secs))
        rc=124
        while kill -0 $qp 2>/dev/null; do
            if grep -q '^RIG-OUT-END' "$R/serial.log" 2>/dev/null; then
                rc=0
                break
            fi
            [ $SECONDS -ge $end ] && break
            sleep 0.1
        done
        case $rc in
        0) echo "aros.sh: done after ${SECONDS}s" >>"$R/run.log" ;;
        *) if kill -0 $qp 2>/dev/null; then
               echo "aros.sh: timeout (the backstop: ${secs}s), the run did not end" >>"$R/run.log"
           else
               echo "aros.sh: QEMU ended after ${SECONDS}s before the commands were done" >>"$R/run.log"
               rc=3
           fi ;;
        esac
        [ "${SHOT_END:-0}" = 1 ] && printf 'screendump %s -f png\n' "$R/shots/end.png" | \
            socat - unix-connect:"$R/mon.sock" >/dev/null 2>&1
        # quit through the monitor, then wait for QEMU to be gone
        printf 'quit\n' | socat - unix-connect:"$R/mon.sock" >/dev/null 2>&1
        wait $qp 2>/dev/null
        trap - EXIT
        exit $rc
    )
    rc=$?
    tr -d '\r' <"$R/serial.log" | sed -n '/^RIG-OUT-BEGIN$/,/^RIG-OUT-END$/p' | sed '1d;$d' >"$R/out.txt"
    rm -f "$R/test.iso"
    cat "$R/run.log" 2>/dev/null
    echo "run dir: $R"
    exit $rc
    ;;
*)
    sed -n '3,44p' "$0"
    exit 2
    ;;
esac

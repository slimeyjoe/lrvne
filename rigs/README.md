# Rigs: the Amiga family's programs on emulated machines

The headless backend is what an engine's tests run on; these rigs run the same engine's
Amiga-family programs on the machines they are for, and compare what those programs show with
the headless build given the same input. They are generic: an engine names its program, its
pack and its options, and gets the presentation test, release archive checks and the pieces
for checks of its own (scripted game runs, audio).

| | |
|---|---|
| `aros/aros.sh` | runs AmigaDOS commands in hosted AROS (x86_64) or the native live CD in QEMU/KVM |
| `aros/check-dist.sh` | a release archive unpacked and run in hosted AROS |
| `aros/dbgtype.c` | guest helper: a QEMU run's output out through the kernel debug channel |
| `winuae/run-lib.sh` | the WinUAE plumbing (sourced): work dirs, startup-sequence, config, snapshot server, the backstop, one run per gdb port |
| `winuae/run-program.sh` | a 68k program in WinUAE: native, Picasso96 or promoted screen, a Workbench in a given mode, AHI, real time or warp |
| `winuae/run-wos.sh` | commands in WinUAE's Blizzard PPC emulation (WarpOS), one run at a time |
| `winuae/check-dist.sh` | a release archive unpacked with LhA and run in WinUAE |
| `winuae/uaequit.c`, `waitwb.c` | guest helpers: quit the emulator; wait for the Workbench in a display mode |
| `snapd.py` | answers a WinUAE guest's snapshot and screenshot requests through the gdbserver |
| `present.py` | the presentation test on hosted AROS, WinUAE or WarpOS against the headless build |
| `lrvrig/` | the Python side: settings, the headless reference, AROS runs, WinUAE runs and their oracle |
| `rig.sh`, `rig.conf.example` | the machine settings |

The make rules (`mk/amiga-rig.mk`) put them behind an engine's build:

    make TARGET=amiga lrvne-present        # WinUAE: native, rtg, promote, uaegfx
    make TARGET=amiga lrvne-present LRVNE_PRESENT_ARGS="--configs window"
    make TARGET=warpos lrvne-present       # WarpOS: native
    make TARGET=aros lrvne-present         # hosted AROS
    make TARGET=amiga|aros lrvne-dist-check
    make TARGET=amiga lrvne-present LRVNE_PRESENT_ARGS="--mode pc98"   # no pack: a machine's mode

lrvne checks its own backend the same way with its demo (`--mode pc98` needs no data).

## Settings

Where the outside pieces are comes from the environment or a rig configuration: `RIG_CONF`,
else `tests/rig.conf` below the directory the engine's Makefile runs in. `rig.conf.example`
lists the keys: `EMU` (the emulator's tree), `WINUAE`, `WINUAE_PPC`, `KICKSTART`, `WBDISK` (a
Workbench 3.1 tree with Picasso96, read only), `WARPUP`, `BPPC_ROM`, `AROS_DIR` (or
`AROS_HOSTED`, `AROS_ISO`, `CONTRIB`), `RIG_WORKTOP` (WinUAE work dirs, default
`$EMU/aos/work/rig`), `RIG_RUNS` (AROS run dirs; the make rules put them in the build
directory). A configuration names this machine's paths: it stays out of version control.

Guest helpers: `make lrvne-rig-tools` (or `make -C rigs/winuae`, `make -C rigs/aros`) with the
targets' compilers on `PATH`.

## Events, not time

A busy host must not change a result, so nothing here waits for a number of seconds, on the
host or in the guest. A guest's seconds are no better: WinUAE at `cpu_speed=max` measures the
CPU's share of a frame in host time, and hosted AROS runs on the host's clock.

* **A snapshot** is the program's request (`backends/amiga/core/snap.c`: written as `.tmp`
  and renamed). On AROS the program reads its screen back itself and goes on. In WinUAE
  `snapd.py` attaches to the gdbserver and works only once the stop reply says the CPU has
  stopped (monitor commands and memory reads are refused while it runs), takes a screenshot,
  reads the displayed bitmap from guest memory, detaches and writes the `.ack`; the program
  waits for it as long as it takes.
* **The Workbench in a display mode** is `waitwb MODEID`.
* **The end of a run** is its last command: `uaequit` in WinUAE; in AROS a marker the host
  waits for (or AROS ending).
* **One backstop per run**, wall clock, for a run that is stuck: `EMU_TIMEOUT` (WinUAE) or
  the `aros.sh` seconds. A run it ended is reported as such: `emu.rc` (124/137) and exit
  status 124. `aros.sh` also exits 3 when AROS ended before the commands were done, 4 when
  the X server could not start.
* **Collisions wait**: runs on one gdb port take turns (a lock per port in `RIG_WORKTOP`;
  WinUAE only logs a port it cannot listen on, and a snapshot server would answer the wrong
  emulator), WarpOS runs take turns (several qemu-uae instances starve each other).
* Host-side waits for a condition (a request file, a marker, the gdbserver listening) poll
  every 0.1 s with the process's life as the other condition: latency, never a verdict.

Conformance runs go in warp (`present.py` winuae; `run-program.sh WARP=1`): FAST,
PRESENTTEST and autoplay count ticks, so the results are the same as in real time, and a busy
host only makes a run longer. `run-program.sh` stays in real time by default, for runs whose
subject is timing (the frame clock, audio interrupts). WarpOS runs in real time: it needs the
68k and the PPC in step. A run with real-time audio (AHI) whose input must meet the program
in the same state as on the host uses an autoplay spec driven by the input waits
(`ok:wait snap:wait waits:N`, `backends/common/autoplay.h`).

## What the machines need

* **WinUAE**: a Linux build with a gdbserver (`debugging_features=gdbserver`, `gdb_port=`)
  that answers `?` with a stop reply once halted and has the `screenshot` monitor command;
  null video is enough. A Kickstart 3.1 A1200 ROM, a Workbench 3.1 tree with Picasso96. For
  audio interrupts a build with SDL3 audio (`AHI=1` installs AHI from `$EMU/aos/ahi/`).
* **WarpOS**: a WinUAE built with qemu-uae's PPC, the Blizzard PPC ROM, WarpUP 5.1.
  `run-wos.sh` sets WarpOS's `Terminator` and `HideWarning` and passes `uaequit` the moved
  boot ROM's address (`RTAREA`, default db0000, as the emulator log prints it).
* **AROS**: the 2026.09 ABIv11 releases: the hosted system (one Linux process; X via
  `xwfb-run`, sound through a private PipeWire sink), for QEMU the pc-x86_64 boot ISO, for
  `check-dist.sh` the contrib archive's `lha`.

## The oracles

`present.py` compares every snapshot with the headless build's frame of the same name
(`lrvrig/frames.py` reads its JSON lines): the frame hash the program computed of what it
presented must be the reference's (else the machine's build computed another frame), and the
picture the reference's colours (each 4-bit DAC value times 17, black with graphics off). On
AROS the picture is the program's own read-back. In WinUAE (`lrvrig/oracle.py`) it is the
emulator's screenshot, cropped at the position calibrated from the presentation test's
registration snapshot (saved as `calibration.json` for later checks), with everything outside
black, and the displayed bitmap read from guest memory, which must hold the frame's colour
indices; scaled screens and windows are expanded and placed as the request says.

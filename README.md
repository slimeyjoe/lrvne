# lrvne — Lightweight Retro Visual Novel Engine

The common low-level layer for re-implementations of retro visual novel
engines.

Engines for old visual novels differ in their script interpreters, picture
formats and music drivers, but underneath they need the same things: a frame
loop paced like the original machine, a planar frame buffer shown on a modern
display, an FM sound chip, fonts, an asset pack, saves, input, and a way to be
tested without a human. lrvne is that common part, written once.

On its own it runs no game and contains nothing of any game. An engine built
on it adds the interpreter and formats of one original engine; a game is then a
*profile* of that engine: data in the game's pack, never code in the engine.

```
   game profile + data      (a pack built from the player's own copy)
   ------------------------------------------------------------------
   engine                   interpreter, formats, music driver of one
                            original engine
   ------------------------------------------------------------------
   lrvne                    src/       portable C99, no I/O, no threads
                            backends/  null (headless), SDL3, Amiga
   ------------------------------------------------------------------
   host
```

## What is in it

| Part | Purpose |
|---|---|
| `include/lrvne/` | the public headers, one per module; each is its module's documentation. `hal.h` is the interface to the host, `app.h` the one between an engine and a backend |
| `src/core` | `sys`: the frame loop with its hooks, the input queue, quit and reset, trace events; `core`: errors, logging, allocation; key names |
| `src/pack`, `src/profile` | the asset pack reader (languages, entries read in ranges) and the game profile parser |
| `src/gfx` | `screen`: pages, dirty rectangles, palette, overlays, presentation; a software mouse pointer; palette fades, the frame hash; planar-to-chunky conversion and integer scaling for backends |
| `src/text` | glyph tables, Shift-JIS and UTF-8, glyph cells into planar pages, reflow of translated text |
| `src/snd` | the OPN (YM2203) cores, the register-write queue that runs a music driver in sample time, the mixer (push and pull delivery), the music path assembled from them, the voice clip player; PMD 4.7g, an optional music driver ([its document](docs/pmd.md)) |
| `src/codec` | LZSS; the voice clip decoders (8-bit PCM, delta codes, Opus) |
| `src/machine/pc98` | the PC-98: its video mode and its keyboard as the originals read it |
| `backends/` | `null` (headless and deterministic, for tests), `sdl3` (the player), `amiga` (AmigaOS 3.x on the 68k, WarpOS, AROS: [its README](backends/amiga/README.md)); `common` is what they share (command line, pack and storage paths, scripted input, snapshots), `control` the test automation protocol |
| `examples/demo` | a small engine that uses all of the above: read it first |
| `tools/` | Python: the pack and font builders, readers of disk and CD images |
| `rigs/` | the Amiga family's programs on emulated machines (WinUAE, WarpOS, hosted AROS) against the headless build: [its README](rigs/README.md) |
| `fonts/`, `third_party/` | the bitmap fonts the glyph tables are built from; ymfm and the Opus decoder |
| `docs/` | [the architecture](docs/architecture.md), [porting an engine onto lrvne](docs/porting-an-engine.md), [the PMD module](docs/pmd.md) |

## Using it in an engine

lrvne is compiled *into* the engine from source; there is no library to
install. Add it as a git submodule and include its make fragment:

```make
LRVNE    := lrvne
TARGET   ?= host
BUILD    := build/$(TARGET)
APP_NAME := myengine
APP_SRCS := $(wildcard engine/*.c engine/*/*.c)
include $(LRVNE)/mk/lrvne.mk

all: $(EXE)
```

`make TARGET=host` builds with the headless backend, `TARGET=sdl3` the player,
`TARGET=amiga`, `warpos` and `aros` the Amiga family's programs with their
cross toolchains. The engine describes itself in a `struct lrv_app` (`include/lrvne/app.h`) and
its `main` is a call to `lrv_app_main`. `examples/demo/demo.c` shows all of it
in one file: a profile and fonts from the pack, the PC-98 screen with a
software pointer, the keyboard translation, FM music through the mixer, a
storage record, trace events. `docs/porting-an-engine.md` is the step-by-step
for an engine that has its own copy of such a layer.

## Building and testing lrvne itself

```
make                 # the demo engine with the headless backend: build/host/lrvdemo
make test            # unit tests, then the demo run headless (tests/smoke.sh)
make test SAN=1      # the same under the address and undefined-behaviour sanitizers
make TARGET=sdl3     # the demo engine in a window: build/sdl3/lrvdemo
make TARGET=amiga    # for AmigaOS 3.x (vbcc); also warpos, aros (backends/amiga/README.md)
make check           # test, the tools' tests, the leak check
make opn68k          # the 68k assembly FM loop against the C one, under qemu-m68k
make TARGET=amiga lrvne-present LRVNE_PRESENT_ARGS="--mode pc98"   # the demo's presentation
                     # test in WinUAE; also warpos, aros (rigs/README.md: the emulators)
```

Build switches (`0` or `1`; changing one rebuilds what depends on it):

| Switch | Default | |
|---|---|---|
| `LRVNE_YMFM` | 1 | the ymfm YM2203 core (C++); the light C core is always built. Off on the Amiga targets |
| `LRVNE_OPUS` | 0 | the Opus decoder for voice clips (vendored libopus, decoder only) |
| `LRVNE_PMD` | 0 | the PMD 4.7g music driver ([docs/pmd.md](docs/pmd.md)); lrvne's own Makefile turns it on for its tests |
| `LRVNE_CONTROL` | 1 | the test automation protocol (`--control`) in the backends. Off on the Amiga targets |
| `SAN` | 0 | sanitizers, on the hosted targets; objects go to `build/<target>-san` |

Requirements: a C99 compiler, GNU Make 4; a C++14 compiler for the optional
ymfm sound core; SDL3 (found with pkg-config) for the `sdl3` target; vbcc and
vasm, or the AROS cross gcc, for the Amiga targets; Python 3 for the tools.

The demo, once built: `lrvdemo --help` lists the options every engine gets
from its backend. `lrvdemo --autoplay "at:5=a,30=esc snap:20"` is a scripted
headless run that prints a snapshot of frame 20; `lrvdemo --control` speaks
the test protocol on stdin and stdout; `lrvdemo --render-wav tune.wav --song
tune` renders the demo's music offline.

## Conventions

- C99, no hidden global state: every module works on a structure its caller
  owns. Failures are `0` / negative `enum lrv_err`, or `NULL`.
- Code above the HAL performs no file I/O, starts no threads, reads no clocks.
- All formats are little-endian and read bytewise; the code runs on big-endian
  hosts.
- No game data in this repository, ever. Tests use synthetic fixtures.
- 4-space indent, 100 columns, `/* */` comments, British spelling, an SPDX
  line at the top of every file.

## Licence

GPL-3.0-or-later (`LICENSE`). Third-party code under `third_party/` and the
fonts under `fonts/` keep their own licences, stated next to them. A few
source files carry a different or additional licence in their SPDX header and
keep it: `src/snd/opn_lite_68k.s` is BSD-2-Clause, `src/snd/opn_lite.c` is
GPL-3.0-or-later AND BSD-3-Clause.

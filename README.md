# lrvne

A low-level layer for re-implementations of retro visual novel engines.

Engines for old visual novels differ in their script interpreters, picture
formats and music drivers, but underneath they need the same things: a frame
loop paced like the original machine, a planar frame buffer shown on a modern
display, an FM sound chip, fonts, an asset pack, saves, input, and a way to be
tested without a human. lrvne is that common part, written once.

It is **not** an engine and contains nothing of any game. An engine built on
it adds the interpreter and formats of one original engine; a game is then a
*profile* of that engine: data in the game's pack, never code in the engine.

```
   game profile + data      (a pack built from the player's own copy)
   ------------------------------------------------------------------
   engine                   interpreter, formats, music driver of one
                            original engine
   ------------------------------------------------------------------
   lrvne                    src/       portable C99, no I/O, no threads
                            backends/  null, SDL3, Amiga family
   ------------------------------------------------------------------
   host
```

## What is in it

| Part | Purpose |
|---|---|
| `include/lrvne/` | the public headers; `hal.h` is the interface to the host, `app.h` the one between an engine and a backend |
| `src/core` | frame loop, input model, trace events, diagnostics |
| `src/pack`, `src/profile` | the asset pack reader and the game profile parser |
| `src/gfx`, `src/text`, `src/snd`, `src/codec` | planar pages and palettes, fonts and text encodings, FM cores and mixing, decoders |
| `src/machine` | constants and key maps of the machines modelled (PC-98) |
| `backends/` | `null` (headless, for tests), `sdl3`, `amiga`; `control` is the test automation protocol |
| `tools/lrvne` | Python: disk and CD image readers, the pack and font builders |
| `examples/demo` | a minimal engine, the in-tree consumer |

## Using it in an engine

lrvne is compiled *into* the engine from source; there is no library to
install. Add it as a git submodule and include its make fragment:

```make
LRVNE    := lrvne
APP_NAME := myengine
APP_SRCS := $(wildcard engine/*.c engine/*/*.c)
include $(LRVNE)/mk/lrvne.mk

all: $(EXE)
```

`make TARGET=host` builds with the headless backend, `TARGET=sdl3` the player.
The engine describes itself in a `struct lrv_app` (`include/lrvne/app.h`) and
its `main` is a call to `lrv_app_main`. `examples/demo` shows all of it.

## Building and testing lrvne itself

```
make test            # unit tests (add SAN=1 for the sanitizers)
make TARGET=sdl3     # the demo engine in a window
make check           # tests, tool tests, leak check
```

Requirements: a C99 compiler, GNU Make 4; a C++14 compiler for the optional
ymfm sound core; SDL3 for the `sdl3` target; Python 3 for the tools.

## Conventions

- C99, no hidden global state: every module works on a structure its caller
  owns. Failures are `0` / negative `enum lrv_err`, or `NULL`.
- Code above the HAL performs no file I/O, starts no threads, reads no clocks.
- All formats are little-endian and read bytewise; the code runs on big-endian
  hosts.
- No game data in this repository, ever. Tests use synthetic fixtures.

## Licence

GPL-3.0-or-later (`LICENSE`). Third-party code under `third_party/` and the
fonts under `fonts/` keep their own licences, stated next to them.

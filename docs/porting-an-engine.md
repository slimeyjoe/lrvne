# Porting an engine onto lrvne

For an engine that already works and carries its own copy of a layer like this
one: a system layer, a screen, a sound path, backends. The aim is to delete
that copy and keep only what is the engine's: the interpreter, the formats,
the music driver. Do the steps in order; after each one the engine builds and
its tests pass. `examples/demo/demo.c` has every call in context, and
`docs/architecture.md` the reasons.

Before starting, record what must not change: snapshot hashes of a scripted
run, a rendered song's checksum, a transcript of a control session.

## 1. Makefile

Add lrvne as a git submodule and replace the engine's compile rules with the
fragment:

```make
LRVNE    := lrvne
TARGET   ?= host
BUILD    := build/$(TARGET)
APP_NAME := myengine
APP_SRCS := $(wildcard engine/*.c engine/*/*.c)
APP_INCLUDES := -Iengine
include $(LRVNE)/mk/lrvne.mk

all: $(EXE)
```

Use `$(BUILD)` and `$(EXE)` only after the include (a sanitizer build gets a
directory of its own). Choose the switches the engine needs (`LRVNE_YMFM`,
`LRVNE_OPUS`, `LRVNE_CONTROL`). The same Makefile builds the engine for every
target of `mk/target/`: `make TARGET=amiga` (or `warpos`, `aros`) needs only
the cross toolchain; `backends/amiga/README.md` says what an engine should
check for it and how its release archives are made. Remove the old layer's sources from `APP_SRCS`
as the steps below replace them.

## 2. main and the app

Delete the engine's `main`, its option parsing and its backend selection.
Describe the engine in a `struct lrv_app` (`lrvne/app.h`): name, version, its
own options with a callback, `run`, optionally `render_music`, the names of
its trace kinds and `wait_is_input`. `main` becomes

```c
int main(int argc, char **argv) { return lrv_app_main(&my_app, argc, argv); }
```

The options every engine has (`--pak`, `--lang`, `--storage`, `--volume`,
`--rate`, `--autoplay`, `--control`, ...) now come from the backend and reach
`run` as a `struct lrv_run_cfg`. Paths are the backend's business: the engine
no longer builds a file name anywhere. The engine's own options are data
(`app->options`), so they also work where options are not spelled `--name`:
on an Amiga `--view` is the keyword `VIEW` of the shell and a tool type of the
icon.

## 3. sys

Replace the engine's frame wait, key queue, mouse state, quit and reset flags
with a `struct lrv_sys` in the engine's state.

- every wait becomes a loop around `lrv_sys_frame_wait`;
- per-frame work moves into hooks: `LRV_HOOK_PUMP` for what runs before the
  present, `LRV_HOOK_TICK` for what the original did in its refresh interrupt;
- a mouse driver's motion counter is `sys.motion_x` / `motion_y`: every poll
  adds to it, the engine clears or subtracts what it used;
- loops test `lrv_sys_quitting`; the top level handles `lrv_sys_resetting`;
- busy loops that the original timed by CPU speed call `lrv_sys_cpu`.

Global variables of the old layer become members of the engine's structure;
lrvne has none.

## 4. Screen

Replace page memory, dirty tracking and the palette with a
`struct lrv_screen` created from the machine's video mode
(`lrv_pc98_video_mode`).

- drawing code takes its target from `lrv_screen_access` or a buffer of the
  screen, and every write is followed by a mark (`lrv_screen_mark`, or
  `lrv_screen_mark_off` where the code addresses video memory by offset, or
  `lrv_screen_mark_surface` for a decoder with a target surface);
- page selection, display on/off and scroll go through the screen's setters;
  what an out-of-range page number means stays the engine's decision;
- palette writes are `lrv_screen_set_colour` / `lrv_screen_set_palette`; the
  timing of a fade stays in the engine, its arithmetic is `lrv_pal_*`;
- anything drawn only for the eye (the mouse pointer, a text layer above the
  graphics) becomes an overlay;
- `lrv_sys_set_present(&sys, lrv_screen_present_hook, &screen)`.

The frame hash is defined by the library now (`lrv_frame_hash32`); compare the
recorded snapshots.

## 5. Sound

The driver stays; its plumbing goes. Create an `lrv_opnq` in front of a core
(`lrv_opn_select(cfg->opn_core)`, its flags `lrv_opn_flags(cfg->opn_core)`) and a
mixer whose main source is `lrv_opnq_render` and whose skip hook is
`lrv_opnq_skip`. Which kind of queue
depends on how the driver keeps time:

- **Clock-driven** (the driver counts the chip's master clocks, typically
  because it models the chip's timers): set `seq.run_until`. It performs
  everything up to and including the clock it is given and stamps each write:
  `lrv_opnq_write_at(q, clock, reg, val)`. Tell the queue which clock output
  sample 0 stands for with `lrv_opnq_set_clock0` after the driver is
  initialised.
- **Frame-driven** (the driver counts output samples until its next event):
  set `seq.advance`. It advances by the frames it is given and calls the
  `span` callback for every run of samples between events; its writes are
  `lrv_opnq_write(q, reg, val)`.

Registers the driver expects to find set go in with `lrv_opnq_poke`. Set
`bus_clocks` to `LRV_OPN_FM_DIV` unless the original's writes were unspaced.

Rates: create the queue first, ask the device for `lrv_opnq_rate` (a core may
insist on its own rate), and if `lrv_mixer_open` answers another rate, try
`lrv_opnq_set_rate`; if the core cannot follow, use `lrv_opn_lite_ops`, which
renders at any rate. Then `lrv_mixer_start`. Register `lrv_mixer_pump` as a
pump hook whatever the delivery model: it does nothing where the backend
pulls. If the game waits on the music even without a device, give the mixer
an `idle` callback that keeps the driver's clock.

Locking: where the backend pulls, the driver runs on its audio thread. Every
call the engine makes into the driver or the queue from the game (start a
song, fade, read its position) is bracketed with `lrv_mixer_lock` /
`lrv_mixer_unlock`, never nested. The headless backend's `--audio pull` aborts
on a nested lock, which finds the mistakes without a sound card.

Offline rendering (`render_music`) is the same path with a mixer that has no
device: `lrv_mixer_start(&m, rate)` and `lrv_mixer_render` in a loop. Compare
the recorded checksum; `gain_floor` selects how the volume rounds.

## 6. Voice

Keep the cue table (which clip belongs to which line) in the engine. Replace
the player with `struct lrv_voice`: `lrv_voice_open` after the sound is set
up, `lrv_voice_pump` as a pump hook, and, when `hal->aux_mode` is
`LRV_AUX_MIX`, `lrv_mixer_set_aux(&mixer, lrv_voice_mix, &voice)`. The engine
says `lrv_voice_play(name, tag)`, `lrv_voice_stop`, asks `lrv_voice_playing`.
A wait for the end of a clip needs a guard against an output that never plays
(`stall_frames`, or `lrv_voice_progress` in the engine's own wait).

## 7. Input translation

The backend delivers host-neutral keys. An engine for a PC-98 original reads
them as the machine did: `lrv_pc98_keypress` turns a press into scan code and
character, `lrv_pc98_keymap` gives the key-down bitmap of a poll. Delete any
table from host key codes to the machine's; if the original read only the
keypad for directions, pass `LRV_PC98_CURSOR_KEYPAD`. Caps and kana are lock
states, never presses to wait for.

## 8. Traces

Replace the engine's event reporting with `lrv_sys_trace` (and `_ints`,
`_items`); an event of another shape is `lrv_sys_trace_begin`, the fields it
needs, `lrv_sys_trace_send`. The kinds every engine shares are in `lrvne/trace.h`: report every
wait with `LRV_TR_WAIT` on entering and leaving, and quit and reset. Number
the engine's own kinds from `LRV_TR_ENGINE`, list their names in
`struct lrv_app`, and say in `wait_is_input` which wait kinds wait for the
player. Set `sys.script_name` / `sys.script_pos` as the interpreter moves, so
every event carries them. The control protocol and scripted input then work
on every hosted backend with no code in the engine.

## 9. Profile

Whatever the engine decided by the game's name or by a compile-time switch
becomes a key of the profile. Declare the engine's profile structure with
`struct lrv_profile_base` embedded, a field table, defaults, and load it with
`lrv_profile_load`. Use `profile.base.id` for `hal->storage_open` and
`profile.base.title` for the window. Optional native parts are named in
`modules` and tested with `lrv_list_has`. When this step is done the engine's
source no longer contains a game's name.

## 10. Packs and tools

Read everything through `struct lrv_pack`: `lrv_pack_find` by name and type in
the current language, `lrv_pack_load` or `lrv_pack_read`. Number the engine's
own entry types from `LRV_PT_ENGINE`. Fonts are `LRV_PT_FONT` entries loaded
with `lrv_font_load`. On the tools' side, build the packs with the pack
builder under `tools/` and keep only the engine's own converters (its picture,
script and song formats) in the engine's repository.

## Afterwards

Delete the old layer and its backends. Run the recorded comparisons once
more, then the engine's tests under `SAN=1` and with `--audio pull`. What is
left in the engine's tree should be the interpreter, the formats, the driver,
and one file that describes the engine to a backend.

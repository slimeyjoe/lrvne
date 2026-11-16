# The architecture of lrvne

lrvne is the part that re-implementations of retro visual novel engines have
in common. This document describes how it is layered, the contracts between
the layers, and how an engine and a game sit on top of it. The headers in
`include/lrvne/` are the reference for every module; `examples/demo/demo.c` is
the whole of it at work in one file.

## Layers

```
   game            a pack: the game's data and its profile (data, never code)
   --------------------------------------------------------------------------
   engine          the interpreter, the formats and the music driver of one
                   original engine; a struct lrv_app
   --------------------------------------------------------------------------
   lrvne   src/    sys     frame loop, input queue, quit/reset, trace
                   gfx     screen: pages, dirty rectangles, palette, overlays
                   text    glyph tables, encodings, glyph cells, reflow
                   snd     FM cores, write queue, mixer, voice player
                   codec   LZSS, voice clip decoders
                   pack, profile, machine/pc98
           ---------------------------------------- struct lrv_hal (hal.h) --
           backends/   null | sdl3 | amiga     + common, control
   --------------------------------------------------------------------------
   host            window system, audio device, files, clock
```

Rules that hold everywhere above the HAL:

- Portable C99 that performs no file I/O, starts no threads and reads no
  clocks. Everything it needs from the host is a function pointer of
  `struct lrv_hal`.
- No hidden state. Every module works on a structure its caller owns
  (`struct lrv_sys`, `struct lrv_screen`, `struct lrv_mixer`, ...), so an
  engine's whole state is one structure of its own that embeds them. The one
  process-wide object is the HAL.
- Failures are `0` / a negative `enum lrv_err`, or `NULL`.
- Memory comes from the HAL (`lrv_alloc`, `lrv_alloc_flags`, `lrv_free`) and is
  released with its size.
- A backend knows no engine and no game; an engine knows no backend. They meet
  in `struct lrv_app` and `struct lrv_hal` only.

The modules of `src/` do not depend on each other more than their subject
requires: `gfx` and `snd` know nothing of `sys`, they merely offer functions of
the hook type (`lrv_screen_present_hook`, `lrv_mixer_pump`, `lrv_voice_pump`)
that an engine registers with it.

## The frame

A frame is the single blocking point of an engine. `lrv_sys_frame_wait` is:

```
   pump hooks        feed audio (lrv_mixer_pump), decode voice ahead, animations
   present           the present callback: lrv_screen_present_hook
   hal->frame_wait   block until the next vertical refresh of the video mode
   frame count + 1
   tick hooks        what the original machine did in its refresh interrupt
   input poll        hal->input_poll into the engine's queue
```

Every wait of an engine (for a key, for a number of ticks, for a fade) is a
loop around it. Busy loops the original ran without waiting are accounted with
`lrv_sys_cpu`, which lets frames pass at the reference machine's speed. Once a
quit or a reset was requested a frame no longer blocks, so the engine's loops
unwind to its top without special cases.

Presentation: the engine draws into the pages of a `struct lrv_screen` and
marks what it wrote. A present applies the overlays (a software pointer, a
text layer: things that exist only in the presented picture), hands the
displayed page to `hal->video_present` as a `struct lrv_frame`, and restores
what the overlays covered. A frame describes itself: the page with its format
and size, the dirty rectangle, the scroll line, the palette with its size and
depth, the display switch. `lrv_frame_hash32` of a frame is the same number on
every backend and host, which is what tests compare pictures by.

## The HAL contract

`include/lrvne/hal.h` is the contract; in short:

- **Video.** `video_open(cfg)` for one video mode; `video_present(frame)` reads
  the page and never keeps a pointer to it. The dirty rectangle covers every
  change of the displayed page since its previous present; a page switch, a
  scroll change or a palette change are not reported as dirty.
- **Time.** `frame_wait` blocks for one refresh of the mode. It is where a
  backend pumps its events.
- **Input.** Keys by position (`lrvne/input.h`), presses in a queue with the
  US layout's character, levels in a bitmap; caps and kana are lock states;
  the pointer absolute or relative; quit, emergency quit and reset as events
  that a flush never drops. Above the HAL, `lrv_sys` keeps the pointer's
  motion in a counter the engine reads and clears as a mouse driver's (either
  kind of pointer), and a flush polls again, so a quit is seen at once.
- **Audio.** Mono 16-bit. *Push*: the engine renders from its frame into
  `audio_space` / `audio_push`. *Pull* (`audio_lock` is not NULL): the backend
  calls the `struct lrv_audio_source` it was opened with from its audio thread
  or interrupt. The source's callbacks take the audio lock themselves; a
  backend calls them without holding it, and the lock need not be recursive.
  The engine brackets its own calls into sound state with the lock. A backend
  that has fallen behind calls `skip` instead of `render`: the same time
  passes, nothing expensive is synthesised.
- **Auxiliary stream (voice).** Either mixed by the library into the main
  stream (`LRV_AUX_MIX`) or pushed to an output of the backend's own
  (`LRV_AUX_CHANNEL`).
- **Storage.** Small named records in the game's own store.
- **Packs.** Ranges of an opaque source; a backend decides where the pack is.
- **Memory, log, fatal, trace.**

`lrv_app_main` is the backend: it parses the command line (its options, the
ones every engine has, the engine's own through `struct lrv_app`), builds the
HAL and calls the engine's `run`. A host with a command line convention of its
own drives the same option tables in its own spelling: the Amiga backend makes
an AmigaDOS argument template and icon tool types of them.

The backends: `null` is headless and deterministic; `sdl3` is the player on
hosts SDL3 runs on; `amiga` is a family, one set of sources for AmigaOS 3.x on
the 68k, WarpOS and AROS, assembled per target from parts (its own screen or a
window, native planar and graphics card displays, Paula and AHI, an audio task
with a CPU budget that uses the audio source's `skip`;
`backends/amiga/README.md`).

## How an engine is structured

```c
struct engine {                 /* everything the engine is */
    const struct lrv_hal *hal;
    struct lrv_sys sys;
    struct lrv_screen screen;
    struct lrv_pack pack;
    struct engine_profile profile;
    struct lrv_mixer mixer;
    struct lrv_opnq opnq;       /* in front of the FM core */
    struct lrv_voice voice;
    /* ... the interpreter, the music driver, the windows ... */
};
```

Its `run` function, in the order the demo has it:

1. open the pack, load the profile (`lrv_profile_load`) and the fonts, select
   the language;
2. `hal->video_open` with the machine's mode and the profile's title;
   `lrv_screen_init`; overlays;
3. `lrv_sys_init`, `lrv_sys_set_present(lrv_screen_present_hook)`;
4. the sound path: sequencer → `lrv_opnq` → core → `lrv_mixer` →
   `lrv_mixer_open`; `lrv_mixer_pump` as a pump hook; the voice player;
5. `hal->storage_open(profile.id)`;
6. the game: a loop that a reset re-enters, built from waits around
   `lrv_sys_frame_wait`, reporting what it does with `lrv_sys_trace`;
7. close in the opposite order.

Sound in more detail. A music driver ("sequencer") stays in the engine; the
library runs it in sample time. `lrv_opnq_render` asks the sequencer to perform
everything up to the end of the block it renders, applies its register writes
at the samples they fall on (spaced as the chip's bus spaced them) and renders
the core in between. A *clock-driven* sequencer counts master clocks and
stamps its writes (`lrv_opnq_write_at`); a *frame-driven* one counts output
samples and has the runs between its events rendered through a callback
(`lrv_opnq_write`, `lrv_opnq_span`). The mixer scales the result, adds the
voice where the backend mixes it, and delivers in the backend's model.

## Game profiles

One engine runs every game of its original engine; what differs between games
is data. The pack's `profile` entry is `key=value` text. The engine declares
the keys it understands as a table of fields over its own profile structure
(`lrvne/profile.h`), sets the defaults and lets the parser fill it; a line the
table does not explain is reported, so a misspelled key is found at once.
Every profile has the base keys: `id` (names the save directory), `title`
(the window), `lang` (the default language) and `modules` (a list the engine
interprets, for optional native parts). Nothing about a particular game is
compiled into an engine or into lrvne. Other META entries use the same syntax;
`lrv_pack_meta_get` reads one key of one without a field table.

## Packs

All of a game's data is one file, an "XPAK" pack: named, typed entries, each
neutral or belonging to a language, read in ranges through the HAL. The format
is specified in `include/lrvne/pack.h`; glyph tables in `include/lrvne/font.h`;
the voice pack and its clips in `include/lrvne/voice.h` and
`include/lrvne/vdec.h`. Packs are built by the Python tools under `tools/` from
the player's own copy of a game. A pack can be a file next to the program or
appended to the executable; `include/lrvne/app.h` lists where a backend looks.

## Testing

Everything is tested without a human and without game data.

- **Unit tests** (`tests/test_*.c`) link the library with a HAL made of
  memory (`tests/test_hal.h`): packs built in the test, a fake audio sink,
  scripted input. `make test SAN=1` runs them under the sanitizers.
- **The null backend** runs an engine headless and deterministic: no real
  time, input from a schedule (`--autoplay`, `backends/common/autoplay.h`),
  snapshots of chosen frames as JSON lines with their hash, pictures, the
  sound into a WAV file, the peak of the allocations. Two runs of the same
  input are the same tick for tick. `tests/smoke.sh` drives the demo this way.
  A schedule can also follow the engine instead of the clock: `ok:wait`
  presses at every wait for the player's input, `snap:wait` takes a snapshot
  there, `waits:N` ends the run after N of them. Such a run compares with a
  machine whose clock differs (an emulator, audio that ends on its own
  device's time) snapshot for snapshot, since nothing in it counts ticks.
- **The presentation test** (`--present-test`, `backends/common/ptest.h`) is a
  fixed sequence of presents through every path of a `video_present`. Its
  snapshot hashes are the reference for a backend with a real display: the
  Amiga backend runs the same sequence on its screen (`PRESENTTEST`) and, on
  AROS, reads every snapshot back from it for comparison. It runs in the mode
  the engine opens, or, with `--present-test-mode pc98` (`PRESENTTESTMODE`),
  in a machine's mode named on the command line without starting the engine,
  so it needs no game data (`lrvne/machine.h`).
- **The control protocol** (`--control`, `backends/control/`) is for tests of a
  whole game. The engine's trace events come out as JSON lines on stdout, and
  the engine stops at well-defined points: when it waits for the player's
  input, at an event with a breakpoint, after a budget of ticks. At a stop the
  driver sends commands on stdin: `press`, `hold`, `mouse`, `snap`,
  `readback`, `break`, `run`. A driver therefore never sleeps and never
  guesses; a transcript of a session replays as an autoplay schedule. The
  protocol knows no engine: an engine names its trace kinds and says which of
  its waits wait for input in its `struct lrv_app`. `backends/control/control.c`
  lists the lines and the commands.
- **Readback.** On a backend that really renders, `readback` returns the
  pixels the renderer produced, to be compared with the engine's page.

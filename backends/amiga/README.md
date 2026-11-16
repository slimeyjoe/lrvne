# The Amiga family's backend

`lrv_app_main` and the HAL for AmigaOS 3.0 and newer on the 68k, for PowerPC
cards under WarpOS and for AROS: three targets built from one set of sources.
The program an engine becomes with it is an ordinary Intuition program: its
own screen (or a window on the Workbench), a window for input, no hardware
taken away from the system except the Paula channels it allocates from
audio.device.

| target | system | toolchain | display | sound |
|---|---|---|---|---|
| `amiga` | AmigaOS 3.x, 68020+ (aimed at a 68040/25 with 2 MB chip and 4 MB fast RAM) | vbcc, vasm | AGA/ECS native, Picasso96 | Paula, AHI |
| `warpos` | the same machines with a PowerPC card, WarpUP 5.x | vbcc (PPC and 68k) | the same | the same |
| `aros` | AROS x86_64, ABIv11 | the AROS cross gcc | graphics card modes | AHI |

Like every backend it knows no engine and no game. Names come from the
engine's `struct lrv_app`, the picture's size and refresh from the video mode
the engine opens, the save directory from the game id it passes to
`storage_open`.

## An engine's Amiga build

An engine that builds with `mk/lrvne.mk` (`docs/porting-an-engine.md`) needs
nothing else:

    make TARGET=amiga          # also warpos, aros

with the toolchain on the `PATH` (`vc` and `vasmm68k_mot` with `VBCC` and the
NDK set up as vbcc's installation describes; `x86_64-aros-gcc` and `AROS_SDK`,
the SDK's `Development` directory, for AROS). The make fragments name no
directory. What the targets decide for the engine:

* `LRVNE_YMFM` and `LRVNE_CONTROL` are off (no C++ compiler; no driver on
  stdin). The FM chip is the light C core, on the 68k with its render loop in
  assembly (`LRVNE_OPN_ASM=0` for the C loop).
* `LRVNE_OPUS=1` builds on all three, with the libopus configuration each
  compiler needs. On a 68040 Opus decodes far slower than real time: a voice
  pack for these machines wants the delta or PCM codecs.
* vbcc writes no dependency files, so every object depends on every header
  (`APP_HDRS` lists engine headers that are not next to its sources).

What an engine should check in its own code: C99 as vbcc takes it (found
here: its `offsetof` takes a plain member, not a member of a member), nothing
that assumes a POSIX host, and a stack use that fits the icon's (below).
`make TARGET=aros` is the quickest test of the last two: a gcc with warnings
and a system one can run on a PC.

**The version string.** The AmigaOS `Version` command looks for `$VER:` text in
the program file. A backend cannot contain an engine's name, so an engine that
wants `version myengine` to answer puts one line into its own source:

    const char my_ver[] = "$VER: myengine 1.0 (15.11.2026)";

**Release archives.** `make TARGET=amiga lrvne-dist` packs the program into an
`.lha` that unpacks to a drawer, with what the engine supplies of icons, ReadMe
parts and other files (`mk/amiga-dist.mk` lists the variables). lrvne has no
icons and no player's ReadMe: an icon is artwork of a game and its tool types
are the engine's choice of defaults, and a ReadMe speaks about one game. A
ReadMe is conveniently kept as parts, the ones all flavours share and each
flavour's own (requirements, installation), which the rule concatenates. No
game data belongs in the archive.

## Layout: parts and flavours

The backend is a set of directories ("parts"); each target's
`mk/target/<TARGET>.mk` lists the ones it is built from (`LRVNE_BACKEND_DIRS`,
all on the include path), so nothing is selected by `#ifdef`:

| part | what | amiga | warpos | aros |
|---|---|---|---|---|
| `core/` | everything that is plain Amiga API: startup and options (`main.c`), packs and saves (`paths.c`), screen and present (`video.c`), the window (`video_win.c`), the uploads `rtg`, `chunky` and `scaled` (`upload_*.c`), input, the frame clock (`timer.c`, `frame.c`, `frameos.c`), the audio task and the AHI driver, snapshots | x | x | x |
| `exec/` | system glue for one CPU running exec (`sys_exec.c`): tasks, semaphores, `Wait`, caches, the E-clock profile clock, the direct frame call | x | | x |
| `warpos/` | system glue for WarpOS instead (`sys_wos.c`: PPC tasks, `WaitPPC`, PPC semaphores, cache flushes, the time base clock, `Run68K`), the OS layout guard, the flavour | | x | |
| `chipset/` | the classic Amiga hardware: the Paula driver and its interrupt handler, the planar chip RAM uploads (plain and through a row map) | x | x | |
| `os3/`, `aros/` | the flavour | x | | x |

The interfaces between them:

* `core/amsys.h`: the system glue, implemented by `exec/` or `warpos/`;
* `core/flavour.h`: what sets a flavour apart, one `flavour.c` per flavour:
  default display mode, rate, where a preloaded pack goes, the frame clock's
  timer unit, the lists of audio drivers and uploads (`AUDIO=` and `UPLOAD=`
  pick from them by name, the first is the default, so a flavour without
  Paula simply does not list it), the stack of the audio process;
* `core/audio.h` (output drivers) and `core/upload.h` (how the page gets into
  a screen buffer);
* `core/amcompat.h`: the only file that tests compiler macros (`IPTR` where
  the headers lack it, vbcc's `__reg`, the startup's Workbench message);
  `core/os_begin.h`/`os_end.h` bracket every block of OS headers (the
  alignment pragma on WarpOS's PPC side, empty elsewhere).

Exactly one flavour macro is set by the target (`LRV_AMIGA_OS3`,
`LRV_AMIGA_WARPOS`, `LRV_AMIGA_AROS`); code tests a flavour only in those two
headers. Pointers never pass through `ULONG` (`IPTR` tag data and `ReadArgs`
results, `uintptr_t` alignment), so a 64-bit flavour compiles the same core.
A new flavour (MorphOS, AmigaOS 4) is a target file, a `flavour.c`, and the
system part it runs on (`exec/` or its own).

The backend's functions and variables are prefixed `am_`. It uses the code the
backends share for the option tables, scripted input and the presentation test
(`backends/common`), but not `backends/common/paths.c`, which is POSIX:
`core/paths.c` is its AmigaDOS counterpart. `ahi.device`'s request is declared
in `core/ahidev.h` from the device's documented interface, so no AHI developer
files are needed to build.

## Options

From a shell the program takes AmigaDOS arguments, from the Workbench the tool
types of its icon; both are generated from the option tables every backend
uses (`backends/common/args.h`): the options every engine has, this backend's
(`core/main.c`), and the engine's own (`app->options`). An option `voice-pak`
is the keyword `VOICEPAK`: upper case, dashes dropped.

    myengine PAK=file LANG=tag WINDOW SCALE=2 AUDIO=ahi ...

In the template a flag is `KEYWORD/S` and an option with an argument
`KEYWORD/K`, so `myengine ?` prompts as any AmigaDOS command does; `HELP`
lists the options with a line about each. As tool types they are `KEYWORD` and
`KEYWORD=value`; one in parentheses, `(QUIET)`, is switched off, the
Information window's convention. An engine option whose argument is optional
takes `KEYWORD=""` in the shell and a bare `KEYWORD` as a tool type for "no
argument". A bad argument is reported in the shell and ends the program; a bad
tool type is skipped (there is nowhere to say so).

| keyword | default | meaning |
|---|---|---|
| `PAK`, `VOICEPAK`, `NOVOICE`, `STORAGE` | see "Packs and saves" | |
| `LANG` | the game's | language tag |
| `VOLUME`, `VOICEVOLUME` | the engine's | percent; Paula's `AUDxVOL` or AHI's request volume where the voice has a stream of its own, else the samples are scaled |
| `RATE` | 11025 (68k, WarpOS), 44100 (AROS) | output sample rate asked for; the driver reports what it really plays. 22050 sounds better but can cost more than a 68040/25 has (see "Audio") |
| `NOAUDIO` | off | no sound at all; `AUDIO=none` is the same |
| `OPN` | `lite` | the only FM core in these builds; `lite2` oversamples its FM part (about 1.6 times the CPU: a 68060) |
| `MODE` | HiRes Laced (`0x8004`) where the page suits it, else `BestModeID` | display mode id of the own screen; Picasso96 promotion of the default is fine |
| `WINDOW`, `PUBSCREEN` | off, the default public screen | the picture in a window on that screen; falls back to the own screen where it cannot be |
| `SCALE` | 1 | page pixels shown 1-3 times as large; `auto` = the largest that fits; on the own screen a graphics card mode |
| `ASPECT` | off | the picture in the shape of the video mode's aspect (rows repeated: 640x400 shown 4:3 becomes 640x480) |
| `UPLOAD` | `rtg` (AROS: `chunky`) | how a graphics card screen is drawn; a planar native screen always takes the CPU copy |
| `AUDIO` | `paula` (AROS: `ahi`) | output driver |
| `AUDIOBUF` | 93 ms | frames per audio block (two blocks in flight = the latency) |
| `AUDIOCPU` | 75 | percent of real time the audio task may spend rendering, 10-100; over it, blocks are skipped instead of starving the machine |
| `AHIUNIT` | 0 | ahi.device unit |
| `PAULA14` | off | 14-bit Paula output on four channels (the voice is then mixed into the music) |
| `VOICEMIX` | off | the library mixes the voice into the music (for an AHI unit with one channel) |
| `PRELOAD` | `auto` | read the whole pack into memory at start: `auto` when it fits in one block with 1.5 MB to spare, `yes` whenever it fits, `no` never |
| `QUIET` | off | no console output |
| `AUTOPLAY`, `MAXTICKS` | | scripted input (`backends/common/autoplay.h`); quote the spec, or `AUTOPLAY=@file` for a long one |
| `FAST`, `SNAPDIR`, `PRESENTTEST`, `PRESENTTESTMODE` | | tests, see "Testing" |

`CONTROL` is in the list because it is in the common table; these builds have
no control protocol and say so.

## Packs and saves

The rules of `lrvne/app.h` in AmigaDOS terms (`core/paths.c`):

    pack      PAK=file; else the pack appended to the program file; else
              <program>.pak in PROGDIR: (<program> = the program file's own
              name, so a renamed copy of an engine finds its game), else the
              only *.pak there (voice packs aside); then the same in the
              current directory.
    voice     VOICEPAK=file; else <pack>_voice.pak beside the pack. NOVOICE:
              none.
    storage   STORAGE=dir; else PROGDIR:saves/<game id>/; else, when that
              cannot be made (a CD, a write-protected disk),
              ENVARC:<app name>/<game id>/; else T:.
    title     what the engine puts in lrv_video_cfg.title: the screen's and
              the window's title.

A record is the file `<key>` in the storage directory, written as `<key>.tmp`
and renamed. A pack appended to the program must leave a file `LoadSeg`
accepts, i.e. be wrapped in a hunk the loader skips (a `HUNK_DEBUG` block)
with the pack's trailer as the file's last bytes; lrvne's tools do not make
such a file yet, a pack next to the program is the tested way.

**Preload.** FFS `Seek` walks the file's extension-block chain (one block per
36 KB on a 512-byte-block volume), forward from the current position or from
the start of the file for a backward seek. With the default 30 buffers nothing
of a multi-megabyte pack's chain stays cached: a seek costs about 25 ms per MB
walked, and the program hitches on asset loads (measured on an A1200 from a CF
card). This is why the pack is read into memory when it fits (`PRELOAD`: fast
RAM on the Amiga, any memory on AROS). Without the preload, `AddBuffers
<volume> 200` (about 100 KB) brings every seek under 2.5 ms, and a read that
continues the previous one skips the `Seek` altogether. A voice pack is never
preloaded; one laid out in the order of play is read forward in short steps.
PFS3 and SFS do not have the problem.

## Keys and the pointer

Keys are delivered by position as the neutral keys of `lrvne/input.h`, from
raw key codes (`core/input.c` has the table); what a key means is the
engine's business. The Amiga keyboard's Help key is `LRV_K_HELP`; the codes
later systems added for PC keyboards (Insert, Page Up/Down, Home, End, F11,
F12, Pause) are in the table too. Caps Lock is the keyboard's own lock.

The backend's own keys: **Shift+Esc** quits at once, **Ctrl+Shift+R** resets,
**right Amiga+K** toggles the kana lock (no Amiga keyboard has such a key),
**F10** in a window steps to its next size. Ctrl-C in the shell and a window's
close gadget ask to quit. All of them take effect at the next frame.

On the program's own screen the pointer is hidden and its motion is reported,
for a game that draws its own; an engine with `app->host_pointer` keeps the
system's pointer and gets its position. In a window the pointer is always
visible and both are reported.

The screen is a normal Intuition screen, so the system shortcuts work:
**LAmiga+N** brings the Workbench to the front, **LAmiga+M** cycles screens.
The backend watches the screen order every frame: when its screen goes to the
back it restores the pointer and stops seeing input; when it comes to the
front again it re-activates its window and hides the pointer. The engine keeps
running in the background (music plays on); nothing is paused.

## Display

`core/video.c`. Shown are `LRV_FMT_PLANAR4` pages whose width is a multiple of
16, up to 1024x480, with up to 16 colours of up to 8 bits a gun; `video_open`
refuses any other mode with `LRV_ERR_RANGE`. The native default mode fits the
640x400 pages of the machines lrvne models; other sizes get the system's best
mode, or `MODE=`.

A custom screen with two V39 `ScreenBuffer`s. The engine owns its planes in
fast RAM; each frame the changed rows and byte columns are copied into the
*displayed* buffer, the way the modelled machines wrote into displayed video
memory, which keeps the engine's frame rate independent of the display's (no
`WaitTOF` per frame). When the engine switches its display page, the whole new
page goes into the hidden buffer and `ChangeScreenBuffer` flips. Which copy
("upload", `core/upload.h`) is used is decided per buffer from `BMF_STANDARD`:

* a planar chip-RAM bitmap takes `planar`, the CPU copy
  (`chipset/upload_planar.c`; the blitter cannot read fast RAM);
* anything else the flavour's graphics card upload: by default `rtg`,
  `BltBitMap` from fast RAM (Picasso96 converts planar to chunky in software),
  or with `UPLOAD=chunky` the page converted to pixels here (`lrvne/p2c.h`) and
  written with one `WriteChunkyPixels` per rectangle, the path for systems
  whose planar blits are slow or broken and the default on AROS.

Palette changes (fades) are `LoadRGB32`s; on a depth-8 screen that is true
colour underneath (its bitmap deeper than the screen) they also re-upload the
whole page, since nothing recolours the pixels already drawn. A scrolled page
(a frame's `scroll_line`) is uploaded whole when the scroll changes; dirty
updates while scrolled go to the rotated rows.

The overscan border is blanked (`VTAG_BORDERBLANK_SET`), so it stays black
instead of showing colour 0 (a fade to white would whiten it). On a graphics
card screen, including a promoted native mode, the screen is reopened with
depth 8 and `BltBitMap` gets a source with four constant extra planes (plane 4
all ones, 5-7 zero): the page's colours are pens 16-31, pen 0 stays black for
the board rows outside the page, and every byte of the chunky buffer is
written (a depth-4 blit leaves the upper four bits of uncleared video memory
in place, which showed as wrong pens). The probe for this happens before any
screen buffer exists: freeing the buffers of a never-flipped promoted screen
crashed the machine it was tried on.

## Window, scale, aspect

Three options on top of the default screen, all chosen once when the display
opens. Without them the plain path runs, untouched by their code except for
one test per present.

**`SCALE` and `ASPECT` on the own screen** (`video.c` `open_scaled`). The page
goes through a row map (`lrvne/scale.h`: nearest neighbour, rows repeated,
never dropped) and, on a graphics card, is drawn `sx` times as wide:

* a native screen takes no larger picture (AGA's wide modes are planar and no
  faster); `ASPECT` there uses the mode's pixel shape from its `DisplayInfo`:
  PAL HiRes Laced is 22:22 (square), so a 640x400 page with a 4:3 aspect
  becomes 640x480 on a 640x480 laced screen, copied by the CPU through the row
  map (`chipset/upload_planar_map.c`); NTSC HiRes Laced is already that shape
  and stays plain;
* a graphics card: square pixels (Picasso96's `DisplayInfo` assumes a 4:3
  monitor for every mode, wrong for the panels these modes run on); the screen
  is the mode given with `MODE=`, else the smallest board mode large enough,
  stepping down to smaller factors when none is. The upload is `scaled`
  (`core/upload_scaled.c`): the changed rectangle expanded a strip of 16 rows
  at a time into a small buffer (never a whole scaled frame) and written with
  `WriteChunkyPixels`.

**`WINDOW`** (`core/video_win.c`): a window on the public screen `PUBSCREEN=`.
The page's colours are 16 exclusive pens (`ObtainPen`); the page is drawn with
`WriteChunkyPixels` into the window's RastPort through the same strips, so the
layers clip it against other windows:

| public screen | result |
|---|---|
| 8 or 16 colours (no 16 free pens) | no window: the own screen opens, with a log line |
| 256 colours (AGA, 8-bit card) | pens changed in place by `SetRGB32`: fades as cheap as on the own screen |
| 15/16/24/32-bit | a changed colour does not reach pixels already drawn, so a palette change redraws the page; only the last of a run while the frame clock is behind, so a fade shows fewer steps instead of stalling |
| no wider than the page | a borderless window (a 640-pixel page on a native HiRes Workbench); smaller than the page: the own screen |

`SCALE`/`ASPECT` choose the window's size; F10 steps through the sizes that
fit with `ChangeWindowBox`. There is one buffer: a page switch redraws the
window in place (it can tear), and `IDCMP_REFRESHWINDOW` redraws the page at
the next present. The click that activates the window is delivered to it too,
so mouse buttons are ignored for two polls after an activation. While
Intuition holds the screen's layers (a window being dragged, a menu open) the
drawing waits and the engine stops; the audio task plays on and the frame
clock catches up.

## Audio

`core/audio.c` runs one audio task (priority 5) that pulls mono 16-bit blocks
from the source the engine gave to `audio_open` (the HAL's pull mode) and
feeds an output driver behind the small interface in `core/audio.h`. Two
blocks are in flight; the task has one block duration (93 ms by default) to
render the next one. The source takes the audio lock itself, a
`SignalSemaphore` (a PPC semaphore on WarpOS); the task calls it without.

**The CPU budget.** FM synthesis can cost more than real time on a 68040/25,
and a priority-5 task that never sleeps starves the engine, Workbench and
every shell. Each rendered block adds its render time minus `AUDIOCPU` percent
of the block duration to a debt; while in debt a block is skipped: the
source's `skip` is called instead of `render` (the HAL's contract: the same
time passes for the music, nothing expensive is synthesised). The task's
average share stays within the budget whatever the song; a song that fits
plays unchanged, one that does not plays with gaps instead of hanging the
machine. At exit the log gives the average and peak render load and the
skipped blocks.

Measured on a real A1200 with a 68040/25 with the light OPN core
(`src/snd/opn_lite.c`): the heaviest of a set of game songs took 154 % of the
CPU at 22 050 Hz and 102 % at 11 025 Hz with the C render loop, 65 % and 42 %
with the assembly loop (`src/snd/opn_lite_68k.s`, bit for bit the same output:
"Testing"). In a game the sequencer, sound effects and the game's own work
come on top, so 11 025 Hz is the 68k default and 22 kHz there is for a 68060.

* **`paula`** (`chipset/audio_paula.c`). Channels come from audio.device
  (`ADCMD_ALLOCATE` at priority 100; an `ADCMD_LOCK` request reports theft,
  after which the program goes silent); then the hardware is driven directly:
  DMA from two chip-RAM blocks, the block interrupt of the first allocated
  channel (`SetIntVector`, restored on exit) re-points the DMA at the other
  block and signals the task. Both stereo sides play the same mono data.
  8-bit: one left and one right channel share a buffer. `PAULA14`: all four
  channels, per side one at volume 64 with the top byte and one at volume 1
  with the next six bits (uncalibrated 14-bit). The rate is rounded to a Paula
  period (11 025 becomes 11 015 Hz) and reported to the engine. The LED filter
  is switched off.
* **`ahi`** (`core/audio_ahi.c`). `ahi.device` is opened at run time only when
  this driver is used, so AHI is not a dependency. Device interface: two
  linked `CMD_WRITE` requests of mono 16-bit samples at the engine's rate; AHI
  resamples and mixes to the unit's mode. For a sound card, or for AHI's
  calibrated 14-bit Paula modes.

**The auxiliary stream (voice).** Where the driver has a second output the HAL
says `LRV_AUX_CHANNEL`: the engine pushes the clip's samples from the main
task into a FIFO, the audio task drains it into two blocks of the second
stream, as long as the main stream's so a render never outlasts one. Commands
(open, close) go to the audio task by signal, so the drivers keep their "audio
task only" rule; a cut silences both blocks at once.

* `paula`: the voice takes the left and right channel the 8-bit music leaves
  free, a second audio.device allocation with its own chip blocks and block
  interrupt. It plays at the Paula rate nearest the clip's (22 030 Hz for
  22 050: 0.09 % slow) and reports the clip's rate, so nothing is resampled;
  8-bit, with `AUDxVOL` as its volume and as the music's (no bits lost to a
  software gain). If another program takes those channels the voice falls
  silent and the engine goes on. `PAULA14` uses all four channels: the voice
  is then mixed into the music (`LRV_AUX_MIX`).
* `ahi`: a second linked `CMD_WRITE` pair on the same unit at the clip's rate;
  ahi.device mixes it with the music and applies the request's volume. A unit
  mixes as many streams as its "Channels" preference allows; with one, the
  voice's requests go to AHI's silent list (they still complete), which is
  what `VOICEMIX` is for. AHI mixes every stream at the unit's frequency: a
  unit below 24 kHz takes the top off a 22 kHz voice.

An output that makes no progress (a stuck driver) is the engine's to guard
against, as on every backend (`lrvne/voice.h`).

An AHI driver on its low-level API (`AHI_AllocAudio`, one channel per chip
voice from the OPN core's `render_split`) would let a card with its own mixer
do the mixing; it is not written.

## Timing and startup

* `core/timer.c`, `core/frameos.c`: timer.device `UNIT_WAITUNTIL` with
  absolute deadlines advanced by the video mode's frame period to the
  nanosecond, so the frame rate does not drift; a stall of more than ten
  frames resynchronises. Each stall prints one line saying where the time
  went since the clock was last on time: pack reads (with the time in `Seek`,
  the first and the largest offset read), presents, the rest, and how much of
  it the audio task ran.
* `core/main.c`: works from the shell (`ReadArgs`) and from the Workbench (the
  startup's message, icon tool types). Fatal errors close the screen and show
  an `EasyRequest`. Nothing gives memory back to an Amiga when a program ends:
  the backend keeps a list of the engine's allocations and releases what is
  left at shutdown, also after a fatal error.
* The main task of an engine in the demo's shape uses a few KB of stack, a
  game engine more (xenon: 6.8 KB). The program sees to its own: on the 68k
  vbcc's startup swaps to a stack of at least 32 KB when the shell (4 KB by
  default) or the icon gave less (`os3/flavour.c` `__stack`; an engine that
  needs more sets `LRVNE_AMIGA_STACK` in its Makefile), on AROS to
  256 KB (`aros/stack_aros.c`); WarpOS runs the engine on the PPC task's
  stack. The log's last lines give the high-water marks of both tasks.

## Memory

The backend's own, for a 640x400 page: two screen buffers of four planes
(250 KB of chip RAM on a native screen, card memory on a graphics card or a
promoted mode), the Paula blocks (4 KB, 8 KB with `PAULA14`, 4 KB more for the
voice), about 35 KB for the sound path and I/O, 105 KB more with AHI
(ahi.device and its driver). The demo engine's 68k program file is 170 KB.
Engine allocations ask for any memory, so without fast RAM everything goes to
chip. At exit the `memory:` line gives the engine's peak by where it landed,
its largest block, what it did not free, and the system's free-memory
low-water mark below what was free at start.

## WarpOS (PowerPC)

`make TARGET=warpos` builds a WarpOS executable (68k startup + PPC code + a
few 68k objects) that runs everything on the PPC and reaches the OS through
amiga.lib's stubs, each a `Run68K` context switch to the 68k.

* **OS structure layout.** vbcc's PPC backend aligns `int` and pointers to 4
  bytes, the OS structures are laid out for the 68k (2 bytes), and neither the
  NDK nor the WarpOS headers say so. Every backend file reads its OS headers
  between `core/os_begin.h` and `core/os_end.h` (`#pragma amiga-align` on the
  PPC, nothing elsewhere); the C library and lrvne's headers stay outside. The
  build compares the layout with the 68k compiler's (`warpos/oslayout.h`,
  `warpos/oslayout68k.c`) at startup and refuses to run on a mismatch.
* **68k code.** Three files are compiled by the 68k compiler
  (`LRVNE_M68K_SRCS` in `mk/target/warpos.mk`) and linked into the PPC
  executable: the Paula interrupt handler (it runs on the 68k), the
  once-per-frame OS call (below) and the layout table. The handler shares its
  state with the driver through a chip-RAM block split by writer
  (`chipset/audio_paula_irq.h`).
* **Few 68k calls.** The main task makes one per frame: `core/frameos.c` does
  the frame clock's timer wait, the IDCMP gather, the front-screen and Ctrl-C
  checks and the free-memory sample in a single `Run68K` (the other builds
  call the same code directly). Palette loads, page flips and RTG blits stay
  separate calls, made only when they happen. The profile clock reads the PPC
  time base; the audio task waits with `WaitPPC` and the audio lock is a PPC
  semaphore, so a Paula block costs no 68k call at all.
* **The audio task** is a PPC task (`CreateTaskPPC` with `TASKATTR_INHERITR2`:
  r2 is the TOC pointer, without it the task hangs on its first global). Its
  OS calls run in a 68k mirror process, which the main task's `Signal()` and
  the Paula interrupt signal. `Forbid()` on the 68k does not hold the PPC
  scheduler, so at the end the main task waits until the task has left
  WarpOS's task list.
* **Caches.** Where the PPC writes what DMA or the 68k reads, the backend
  flushes the PPC data cache explicitly (sample blocks, the chip planes after
  a CPU upload, the engine's pages before a `BltBitMap`); where it reads what
  the handler wrote, it invalidates that cache line first.

The 68k assembly FM loop is not used; defaults are the 68k build's.

## AROS (x86_64)

`make TARGET=aros` builds from `core/`, `exec/` and `aros/` for ABIv11. What
the flavour changes:

* sound only through AHI, at 44 100 Hz;
* no Amiga display modes: `BestModeID` for the page at depth 8, and the page
  centred on a larger screen. Depth-8 screens are true colour underneath, so
  every palette change re-uploads the page; the `chunky` upload is the
  default;
* the frame clock on `UNIT_MICROHZ` (the time left to the same absolute
  deadline): `UNIT_WAITUNTIL` returns in 20 ms steps there;
* preload into any memory (natively all RAM is `MEMF_CHIP`);
* a 256 KB stack whatever the start gave (`aros/stack_aros.c`,
  `NewStackSwap`: hosted AROS's X11 driver runs Xlib on the caller's stack and
  brought the system down with the 64 KB an icon gave), the audio process
  256 KB too;
* snapshots read back from the screen by the program itself
  (`aros/snap_aros.c`).

## Testing

No test of these targets needs a human or game data.

* **The presentation test.** `PRESENTTEST` runs the sequence of
  `backends/common/ptest.h` in the video mode the engine opens: every path of
  a present, a snapshot after each step. `PRESENTTESTMODE=pc98` runs it in that
  machine's mode without starting the engine, so no pack is needed. The
  headless backend prints the same snapshots (`--present-test`,
  `--present-test-mode pc98`), which are the reference.
* **Scripted runs.** `AUTOPLAY=spec` plays an input schedule; its `snap:`
  ticks take snapshots. `FAST` drops the frame pacing, `MAXTICKS` ends a run.
  `ok:wait snap:wait waits:N` follows the engine's input waits instead of the
  ticks (`backends/common/autoplay.h`): for a run with real-time audio, whose
  clips end on the device's clock, against the headless reference.
* **Snapshots** (`core/snap.c`). For each one the program writes a request
  file to `SNAPDIR` with the frame's hash (`lrv_frame_hash32`, the number the
  headless backend prints) and where the displayed bitmap is in memory. On
  AROS it writes the picture too, read back from the screen
  (`ReadPixelArray`), and goes on. On the other two it waits for an
  acknowledgement file: a rig around an emulator with a debugger stub stops
  the machine, takes the screenshot and the bitmap, and answers. The program
  waits for that answer without a deadline of its own (the rig's timeout is
  the only one; Ctrl-C ends the wait), and after every snapshot the frame
  clock starts again from now, so the pause is not counted as a stall.
* **The assembly FM loop.** `make opn68k` (`tests/opn68k.sh`) links the
  assembly loop and the C loop into one 68k program and runs it under
  qemu-m68k as a 68040: the same pseudo-random song at four rates, every
  sample compared. The Amiga build proves the structure offsets the assembly
  hard-codes at compile time (`src/snd/opn_lite.c`).

What has been run in this form: the AROS build in hosted AROS on Linux,
headless (the presentation test on the own screen, with `ASPECT`, with
`SCALE=2 ASPECT` and in a `WINDOW` on a true colour screen: every snapshot
equal to the headless backend's, pixel for pixel; `PRESENTTESTMODE=pc98`
with no pack, the same; scripted runs; pack
discovery; storage; the options); the assembly loop as above. The 68k and
WarpOS programs build and link; they have not been run since the backend was
generalised. Before that, as part of one engine, the same code ran on a real
A1200 with a 68040, in WinUAE (native, promoted and graphics card screens,
Paula and AHI, the Blizzard PPC emulation for WarpOS), on a PowerPC board
under a WarpOS-compatible system with AHI, and in AROS natively under QEMU.
Not tested there either: a Blizzard or CyberStorm PPC board, Paula output
from the PPC, the PPC cache handling on real hardware.

## Not done / ideas

* The AHI low-level driver (above); a `SCREENMODE` requester (ASL) instead of
  `MODE=`; `SCALE=auto` as the AROS default.
* Pausing when the screen is in the back, interleaved bitmaps and a chip-RAM
  shadow for faster native uploads, `ScrollVPort` for a scrolled page.
* A tool that appends a pack to an Amiga program file; offline rendering of a
  song (`render_music`), which the hosted backends offer.
* `WaitBOVP` before a full window redraw on a native screen (less tearing);
  the activating-click guard for the own screen too; one `MakeScreen` instead
  of 16 `SetRGB32`s per palette change in a window on AGA.
* MorphOS and AmigaOS 4 flavours.

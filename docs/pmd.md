# The PMD module

PMD is M. Kajihara's music driver for the PC-98, and many games of the 1990s played their
music through it. lrvne has a reimplementation of PMD 4.7g as its build for the YM2203 plays
(`PMD.COM`, for the PC-9801-26K board and compatibles), so that every engine whose game used
it shares one. It is an optional module: an engine switches it on with `LRVNE_PMD=1`
(`mk/lrvne.mk`) and includes `lrvne/pmd.h`.

## What it is

The module is the resident driver, not a player of songs: it does what PMD.COM does, register
write for register write, when the game calls it. It has:
- the part interpreter: FM parts A–C, the FM3 parts D–F on the slots of channel 3, SSG parts
  G–I, with every command of the format, the LFOs, the software envelopes, portamento, the
  masks and the volume-down;
- the FM effect part, which plays effects from an effect bank (an `.EFC` file);
- PMD's built-in SSG effects;
- the fade;
- the chip's two timers, which clock all of it: Timer B is the music clock, Timer A the
  effect clock. The timers run from their register values: a song's tempo is the Timer-B
  value its commands set.

PMD.COM addresses in the source (CS offsets) name the routines a function reimplements.

## Using it

The driver is the frame-driven sequencer of the music path. `lrvne/music.h` assembles that
path, and the driver writes into its queue:

```c
static void pmd_write(void *ctx, uint8_t reg, uint8_t val)
{
    lrv_opnq_write(ctx, reg, val);                  /* ctx: the music's queue */
}

    struct lrv_music_cfg mc = { 0 };
    lrv_pmd_seq(&e->pmd, &mc.seq);
    mc.core = cfg->opn_core;
    mc.rate = cfg->audio_rate;
    mc.clock_hz = LRV_OPN_CLOCK_PC98;
    mc.bus_clocks = LRV_OPN_FM_DIV;     /* PMD waits for the chip between two writes */
    mc.queue_size = 1024;               /* a song start writes about 200 registers */
    mc.queue_flags = LRV_OPNQ_FLUSH_ON_FULL;
    mc.idle_rate = 44100;               /* the music's time passes without a device */
    mc.frame_num = 5642;                /* the frame: 56.42 Hz */
    mc.frame_den = 100;
    lrv_music_open(&e->music, hal, &mc);
    lrv_mixer_lock(&e->music.mixer);
    lrv_pmd_init(&e->pmd, pmd_write, &e->music.q, lrv_music_rate(&e->music));
    lrv_pmd_set_ssgtab(&e->pmd, table, table_size);    /* if the game uses SSG effects */
    lrv_mixer_unlock(&e->music.mixer);
    lrv_music_start(&e->music);
```

The engine owns what the original engine owned, and the driver reads and writes it in place:
- **The music buffer.** The original engines copied a song file into PMD's buffer, and so does
  the port: `lrv_pmd_load_music` gets the song from its byte 1 on (byte 0, the sound-source
  type, is not passed). Loop counters live in the song, so the buffer is mutable.
- **The effect bank**, when the game has one (`lrv_pmd_set_efc`). It is mutable too.
- **The SSG-effect table** (`lrv_pmd_set_ssgtab`). It is part of PMD.COM and so the player's
  own: `tools/lrvne/pmd.py` takes it out of their PMD.COM 4.7g when the pack is built.

The driver keeps pointers to all three. Every call into it from the game is made under the
audio lock (`lrv_mixer_lock`), because in pull mode it runs on the backend's audio thread.

## INT 60h

| function | AH | what it is |
|---|---|---|
| `lrv_pmd_mstart` | 00h | start the song in the music buffer |
| `lrv_pmd_mstop` | 01h | stop it |
| `lrv_pmd_fade` | 02h | fade out at a speed |
| `lrv_pmd_ssg_effect_start` / `_stop` | 03h / 04h | a built-in SSG effect |
| `lrv_pmd_get_fadevol` | 08h | the fade volume |
| `lrv_pmd_get_status` | 0Ah | status1 (set by the song) and status2 (its loop count; FFh when it has ended) |
| `lrv_pmd_fm_effect_on` / `_off` | 0Ch / 0Dh | an FM effect from the bank |
| `lrv_pmd_set_fadevol` | 19h | set the fade volume |

## Not covered

- The OPNA builds (PMD86, PMDB2): FM channels 4–6, the rhythm source, ADPCM and 86-PCM, pan.
  These need a YM2608 core in lrvne first (`docs/roadmap.md`).
- PPSDRV (SSG samples through PDR).
- PCM part J, which PMD.COM never plays.
- The hot keys, skipping to a measure, and command `C6`.

## How it is checked

`tests/test_pmd.c` replays logs of the real driver. `tests/data/pmd/README.md` describes them:
songs of our own (`tools/lrvne/pmd.py` writes them), run by PMD.COM 4.7g under an emulator with
a pretend 26K board, interrupt by interrupt. For every interrupt the test checks the same
register writes, in the same order, at the same master clock, and the same status after it.
It also checks that advancing in output samples puts each write on the sample of its clock.

The engines that use the module compare it with recordings of their games' songs, played by
the real driver.

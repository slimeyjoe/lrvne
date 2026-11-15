# Roadmap: what engines not yet built on lrvne would need

lrvne grows by need: a feature is added when an engine uses it, not before.
This is the list of needs already known from surveys of further PC-98 era
engines, so that interface changes can be weighed before 1.0. Nothing here is
implemented.

| Need | Size | Effect on existing interfaces |
|---|---|---|
| Palette helpers: add-and-clamp shift of all components; a step that only decrements | small | additive (`gfx.h`) |
| Font builder input from a raw glyph array (a font embedded in a program file) | small | additive (`tools/lrvne/xfont.py`) |
| Glyph helpers: plain OR bold (no gap kept), transparent OR, inverse, outline, double size | small | additive (`font.h`) |
| EXEPACK unpacker for program files that hold game data | small | additive (tools) |
| YM2608 (OPNA) core: six FM channels, SSG, ADPCM-B; the vendored ymfm contains it | medium | a new ops structure and queue variant is additive; a register-bank bit in `opn.h` / `opnq.h` would change them |
| A PCM channel in the sound path, for sample playback beside the FM chip (not voice clips) | medium | additive (`mixer.h`) |
| Stereo output: HAL `audio_open`, mixer, every backend | large | changes interfaces; decide before 1.0. A mono downmix of a stereo chip needs none of it |
| Disk images and filesystems of other machines (X68000 DIM, Human68k) | small | additive: an image kind and a filesystem probe |
| A second machine module (X68000: its video mode, keyboard map, YM2151 core) | large | mostly additive; shaped for by `lrv_video_mode` and `src/machine/` |
| A tick source that is not the frame (a sound driver's timer pacing the game) | medium | additive to `sys.h` |
| Joystick input | small | adds to `input.h` and every backend |
| A music driver used by more than one engine as an optional module | medium | additive; a change of scope: today sequencers stay in the engines |

Engine-side matters the surveys turned up, for which lrvne already has the
means: a keyboard handler that keeps only the last key (collapse the press
queue), keys that register on release (the key-down map's edge), effects timed
by CPU speed (`lrv_sys_cpu`, with a reference speed that has to be measured),
and interpreters that poll a music driver in a loop with no wait (charge CPU
work per statement so time passes).

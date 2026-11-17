# Roadmap: what engines not yet built on lrvne would need

lrvne grows by need: a feature is added when an engine uses it, not before.
This is the list of needs already known from surveys of further PC-98 era
engines, so that interface changes can be weighed before 1.0. Nothing here is
implemented.

| Need | Size | Effect on existing interfaces |
|---|---|---|
| Glyph helpers: plain OR bold (no gap kept), transparent OR, inverse, outline, double size | small | additive (`font.h`) |
| YM2608 (OPNA) core: six FM channels, SSG, ADPCM-B; the vendored ymfm contains it | medium | a new ops structure and queue variant is additive; a register-bank bit in `opn.h` / `opnq.h` would change them |
| A PCM channel in the sound path, for sample playback beside the FM chip (not voice clips) | medium | additive (`mixer.h`) |
| Stereo output: HAL `audio_open`, mixer, every backend | large | changes interfaces; decide before 1.0. A mono downmix of a stereo chip needs none of it |
| Disk images and filesystems of other machines (X68000 DIM, Human68k) | small | additive: an image kind and a filesystem probe |
| A second machine module (X68000: its video mode, keyboard map, YM2151 core) | large | mostly additive; shaped for by `lrv_video_mode` and `src/machine/` |
| A tick source that is not the frame (a sound driver's timer pacing the game) | medium | additive to `sys.h` |
| Joystick input | small | adds to `input.h` and every backend |
| PMD's OPNA builds (PMD86, PMDB2) in the PMD module: FM 4–6, the PCM part J, pan | medium | additive; needs the YM2608 core and the PCM channel above |
| PPSDRV in the PMD module: the drums of part K as SSG samples | small | additive; needs the PCM channel above |

Engine-side matters the surveys turned up, for which lrvne already has the
means: a keyboard handler that keeps only the last key (collapse the press
queue), keys that register on release (the key-down map's edge), effects timed
by CPU speed (`lrv_sys_cpu`, with a reference speed that has to be measured),
and interpreters that poll a music driver in a loop with no wait (charge CPU
work per statement so time passes).

Done since the surveys: the font builder takes a raw glyph array, a font embedded in a
program file (`xfont` `add_raw`, `--raw`); `tools/lrvne/exepack.py` undoes EXEPACK on the
program files that hold game data. The palette helpers: an add-and-clamp shift of every
component (`lrv_pal_shift`) and a step that only decrements (`lrv_pal_step_down`).
Reflow: a translated passage can continue a page the engine already began
(`lrv_reflow_layout_from`: its first line, a start column after a speaker's name, a hanging
indent, indentation kept), with `tools/lrvne/reflow.py` as the Python reference.

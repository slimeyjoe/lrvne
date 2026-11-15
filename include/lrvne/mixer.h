/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mixer: owns the HAL's audio stream and delivers what the engine's sound sources render.
 *
 * One main source (the music: lrv_opnq_render of lrvne/opnq.h, or anything with that
 * signature) is rendered in blocks, scaled by the main volume, and the optional auxiliary
 * source (voice clips, lrvne/voice.h, where the backend's aux_mode is LRV_AUX_MIX) is added to
 * it, saturating. Everything is mono 16-bit.
 *
 * Delivery follows the backend (lrvne/hal.h):
 *   push   lrv_mixer_pump, which the engine registers as an LRV_HOOK_PUMP hook, asks the sink
 *          once a frame how much it can take, renders that much and pushes it.
 *   pull   the backend's audio thread or interrupt calls the mixer's callbacks (the struct
 *          lrv_audio_source it was opened with), which take the audio lock for the block.
 *          The pump does nothing.
 * In pull mode the sources run on the backend's thread, so every call the engine makes into
 * their state (the sequencer, the write queue, a clip's ring) is bracketed with
 * lrv_mixer_lock / lrv_mixer_unlock, which are no-ops in push mode. The lock is never taken
 * twice: nothing called with it held takes it again, and the backend does not hold it when it
 * calls the mixer.
 *
 * A pulling backend that has fallen behind may skip a block instead of rendering it
 * (lrv_audio_source.skip): the mixer then lets the same time pass in its sources, through the
 * main source's skip hook where it has one, so the music is silent for the block but neither
 * the sequencer nor a clip loses its place.
 *
 * Without an audio device the mixer still works as a renderer: lrv_mixer_render gives the
 * same blocks to whoever asks (an offline render of a song; a silent clock kept by the idle
 * callback so that the game's waits on the music still end). */
#ifndef LRVNE_MIXER_H
#define LRVNE_MIXER_H

#include <stdint.h>
#include "lrvne/core.h"

#define LRV_MIXER_BLOCK 2048        /* the most frames rendered in one piece when pushing */

/* The main source: writes `frames` samples. */
typedef void lrv_mixer_render_fn(void *ctx, int16_t *mono, int frames);
/* The auxiliary source: adds itself to `frames` samples that play at `rate`. */
typedef void lrv_mixer_aux_fn(void *ctx, int16_t *mono, int frames, uint32_t rate);

struct lrv_mixer_cfg {
    lrv_mixer_render_fn *render;    /* the main source; NULL = silence */
    /* optional: the main source for a skipped block. It advances exactly as render does for
     * `frames` samples without synthesising them and writes silence (lrv_opnq_skip). NULL =
     * the source cannot: a skipped block is rendered like any other. */
    lrv_mixer_render_fn *skip;
    void *ctx;                      /* of render and skip */
    lrv_mixer_aux_fn *aux;          /* NULL = none; lrv_mixer_set_aux changes it later */
    void *aux_ctx;
    /* optional: called by the pump once a frame while there is no audio device, for an engine
     * whose music driver must keep time anyway */
    void (*idle)(void *ctx);
    void *idle_ctx;
    int gain_floor;                 /* how the main volume rounds a sample: 0 = towards zero
                                     * (s * g / 256), 1 = down (s * g >> 8) */
};

struct lrv_mixer {
    const struct lrv_hal *hal;
    struct lrv_mixer_cfg cfg;
    uint32_t rate;                  /* the rate the sources render at; 0 = not started */
    int open;                       /* the HAL's stream is open */
    int pull;                       /* ... and the backend pulls */
    int live;                       /* started: the render callback may run the sources */
    int volume;                     /* main volume, percent 0..100 */
    int32_t gain;                   /* what the mixer itself applies, x/256 */
    int16_t buf[LRV_MIXER_BLOCK];   /* the pump's block */
};

/* The mixer of a HAL, without a stream yet; the main volume is 100. */
void lrv_mixer_init(struct lrv_mixer *m, const struct lrv_hal *hal,
                    const struct lrv_mixer_cfg *cfg);

/* Opens the HAL's audio stream at `rate` and returns the rate the backend accepted, 0 when
 * there is no audio. The stream is silent until lrv_mixer_start: a pulling backend may call
 * the render callback from now on, and the sources may still have to be set up for the rate
 * that was accepted. */
int  lrv_mixer_open(struct lrv_mixer *m, int rate);
/* The sources are ready and render at `rate` (0 = the rate the backend accepted). Also starts
 * a mixer without a device, for lrv_mixer_render. */
void lrv_mixer_start(struct lrv_mixer *m, uint32_t rate);
/* Stops the sources being called and closes the stream. */
void lrv_mixer_close(struct lrv_mixer *m);

/* 1 when a stream is open; the rate the sources render at (0 before lrv_mixer_start). */
int  lrv_mixer_has_device(const struct lrv_mixer *m);
uint32_t lrv_mixer_rate(const struct lrv_mixer *m);

/* The audio lock, for the engine's calls into what the sources read. */
void lrv_mixer_lock(const struct lrv_mixer *m);
void lrv_mixer_unlock(const struct lrv_mixer *m);

/* The main volume in percent (clamped to 0..100; 0 is silence). It is the backend's to apply
 * when its set_volume takes LRV_VOL_MAIN, else the mixer scales the main source's samples.
 * Takes the lock itself. */
void lrv_mixer_set_volume(struct lrv_mixer *m, int percent);
/* Installs or removes (NULL) the auxiliary source. Takes the lock itself. */
void lrv_mixer_set_aux(struct lrv_mixer *m, lrv_mixer_aux_fn *aux, void *ctx);

/* Push mode: renders what the sink can take now and pushes it. An lrv_hook_fn (lrvne/sys.h);
 * ctx is the mixer. */
void lrv_mixer_pump(void *m);

/* Renders the next `frames` samples of the mix, without locking and without a device: main
 * source, volume, auxiliary source. Silence before lrv_mixer_start. */
void lrv_mixer_render(struct lrv_mixer *m, int16_t *mono, int frames);
/* The same span of time as a skipped block: the main source's skip hook in place of its
 * render (its silence is not scaled), the auxiliary source as always. */
void lrv_mixer_skip(struct lrv_mixer *m, int16_t *mono, int frames);

/* ---- sample helpers --------------------------------------------------------------------- */

static inline int16_t lrv_sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Scales n samples by gain / 256 (0..256); floor selects the rounding as in lrv_mixer_cfg. */
void lrv_pcm_gain(int16_t *pcm, int n, int32_t gain, int floor);

#endif

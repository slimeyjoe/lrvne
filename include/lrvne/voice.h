/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The voice clip player: plays one clip of the voice pack at a time on the HAL's auxiliary
 * stream.
 *
 * The voice pack (LRV_PACK_VOICE) holds the clips (LRV_PT_VOICE entries, lrvne/vdec.h) and a
 * "voice" entry (LRV_PT_META) of key=value lines: `codec` (pcm8, ldp4, ldp6, opus), `rate`
 * (the clips' sample rate) and whatever else the pack's builder and the engine agree on, such
 * as the identity of the cue table the clips were named from. Which clip belongs to which line
 * of a script is the engine's business: it keeps the cue table and tells the player "play the
 * clip named X", "cut", and asks "is it still playing".
 *
 * A clip is decoded on the engine's thread by lrv_voice_pump, a little ahead of the output,
 * into a ring of samples at the clip's rate; the output takes it from there:
 *   LRV_AUX_MIX      lrv_voice_mix, the mixer's auxiliary source (lrvne/mixer.h), resamples it
 *                    to the stream's rate and adds it to the music, under the audio lock (in
 *                    pull mode on the backend's audio thread);
 *   LRV_AUX_CHANNEL  lrv_voice_pump resamples it to the rate the backend's aux output accepted
 *                    and pushes it.
 * Pack reads therefore never happen on an audio thread. In MIX mode every access to the ring
 * and the play state takes the audio lock; in CHANNEL mode there is one thread.
 *
 * Everything is a no-op (and lrv_voice_enabled is 0) unless lrv_voice_open succeeded. */
#ifndef LRVNE_VOICE_H
#define LRVNE_VOICE_H

#include <stdint.h>
#include "lrvne/core.h"
#include "lrvne/pack.h"
#include "lrvne/vdec.h"

#define LRV_VOICE_RING  8192        /* samples at the clip's rate (0.34 s at 24 000 Hz); 2^n */
#define LRV_VOICE_LEAD  4096        /* decoded this far ahead of the output */
#define LRV_VOICE_CHUNK 512         /* samples decoded or pushed in one piece */

enum lrv_voice_event {
    LRV_VOICE_EV_START = 0,         /* a clip started */
    LRV_VOICE_EV_CUT,               /* ... was cut (lrv_voice_stop, another clip, the guard) */
    LRV_VOICE_EV_END                /* ... was played to its end */
};

struct lrv_voice_cfg {
    int volume;                     /* percent 1..100 (anything else: 100) */
    int default_rate;               /* the rate of a pack whose "voice" entry names none; 0 =
                                     * such a pack is refused */
    /* The "no progress" guard: an output that plays nothing (a stuck driver, an emulator
     * without audio DMA) must not hold a game that waits for the clip. A clip whose output
     * has not advanced for this many pumps in a row is cut. 0 = no guard in the pump: the
     * engine watches lrv_voice_progress in its own wait. */
    int stall_frames;
    /* LRV_AUX_MIX without an audio stream (nothing ever calls lrv_voice_mix): the pump plays
     * the clip out silently in real time, clip rate * silent_den / silent_num samples a pump,
     * silent_num / silent_den being the pumps (frames) per second, so the clip lasts as long
     * as it would be heard. 0 = not; never set it when a stream may mix the clip. */
    uint32_t silent_num, silent_den;
    /* optional: told of every enum lrv_voice_event with the clip's name and the tag given to
     * lrv_voice_play (for the engine's trace) */
    void (*event)(void *ctx, int ev, const char *clip, int tag);
    void *ctx;
};

struct lrv_voice {
    const struct lrv_hal *hal;
    struct lrv_voice_cfg cfg;
    int on;                         /* open: the voice pack and an output are there */
    int mode;                       /* LRV_AUX_MIX / LRV_AUX_CHANNEL */
    struct lrv_pack pak;            /* the voice pack */
    char codec[16];                 /* its "voice" entry's codec= */
    int pack_rate;                  /* ... and rate=, or cfg.default_rate */
    int gain;                       /* 0..256, applied to the decoded samples */
    uint32_t ch_rate;               /* CHANNEL: the rate the backend accepted */

    int playing;                    /* a clip is decoding or still being played out */
    int dec_done;
    int end_sent;                   /* CHANNEL: the end-of-clip push has been made */
    struct lrv_vdec dec;
    char clip[LRV_PACK_NAME_LEN + 1];
    int tag;

    int16_t *ring;                  /* LRV_VOICE_RING samples */
    uint32_t rhead, rtail;          /* free-running: rhead - rtail = samples queued */
    /* the resampler (the one consumer of the ring) */
    uint32_t step;                  /* clip rate / output rate, 16.16 */
    uint32_t step_rate;             /* the output rate the step was computed for */
    uint32_t frac;
    int16_t s0, s1;
    int primed;

    uint32_t silent_acc;            /* silent play-out: samples / silent_num owed */
    uint32_t last_progress;         /* the guard */
    int still;
    unsigned long n_ev[3];          /* clips started, cut, ended (enum lrv_voice_event) */
    int16_t tmp[LRV_VOICE_CHUNK];   /* decode / output scratch of the engine's thread */
};

/* Opens the voice pack and the output. Returns LRV_OK, or
 *   LRV_ERR_NOTFOUND  the backend has no auxiliary stream, or no voice pack (not an error:
 *                     the game runs without voice)
 *   LRV_ERR_FORMAT    the pack's codec is not decoded by this build (v->codec names it), or
 *                     the pack states no rate and there is no default
 *   LRV_ERR_IO        the backend could not open its aux output
 *   LRV_ERR_NOMEM
 * and the player stays off. Call it after the sound is set up. */
int  lrv_voice_open(struct lrv_voice *v, const struct lrv_hal *hal,
                    const struct lrv_voice_cfg *cfg);
/* Cuts a playing clip, closes the output and the pack. */
void lrv_voice_close(struct lrv_voice *v);
int  lrv_voice_enabled(const struct lrv_voice *v);

/* The value of `key=` in the voice pack's "voice" entry, "" when it has none. Returns the
 * length stored (at most cap - 1). */
int  lrv_voice_meta(const struct lrv_voice *v, const char *key, char *out, int cap);

/* The voice volume in percent 1..100; in CHANNEL mode it is the backend's to apply when its
 * set_volume takes LRV_VOL_AUX. Applies to what is decoded from now on. */
void lrv_voice_set_volume(struct lrv_voice *v, int percent);

/* Cuts whatever plays and starts the clip named `clip` (an LRV_PT_VOICE entry); `tag` is the
 * caller's own (a cue's part, a string's offset) and comes back in the events. The first
 * samples are decoded here, so the clip is audible from the next render on. Returns LRV_OK,
 * LRV_ERR_NOTFOUND (no such clip), LRV_ERR_FORMAT (not a clip this build decodes), or
 * LRV_ERR when the player is off. */
int  lrv_voice_play(struct lrv_voice *v, const char *clip, int tag);
/* Cuts whatever plays, at once. */
void lrv_voice_stop(struct lrv_voice *v);
/* 1 while a clip is decoding or still being played out. */
int  lrv_voice_playing(const struct lrv_voice *v);
/* How far the output has got: a count that advances while the clip is really being played
 * (samples taken from the ring, minus what the backend still holds). For a wait's own guard. */
uint32_t lrv_voice_progress(const struct lrv_voice *v);

/* Once a frame: decodes ahead, feeds the CHANNEL output, notices the end of the clip. An
 * lrv_hook_fn (lrvne/sys.h); ctx is the player. */
void lrv_voice_pump(void *v);
/* LRV_AUX_MIX: adds the clip to `frames` samples playing at `rate`, saturating. The mixer's
 * auxiliary source (an lrv_mixer_aux_fn; ctx is the player), called with the audio lock held. */
void lrv_voice_mix(void *v, int16_t *mono, int frames, uint32_t rate);

#endif

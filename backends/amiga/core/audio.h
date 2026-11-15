/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Audio output drivers of the Amiga backend.
 *
 * The backend runs one audio task (audio.c) that owns the output device for the whole
 * session. The engine never sees the device: the task pulls mono 16-bit blocks from the
 * source the engine gave to audio_open (struct lrv_audio_source, lrvne/hal.h) and hands them
 * to one of these drivers, which converts and queues them. Two blocks are in flight (double
 * buffering): while the device plays one, the task fills the other, so the task has one block
 * duration to render a block.
 *
 * Every function here is called from the audio task only, in this order:
 *   open -> [block_buffer] fill, submit (block 0), fill, submit (block 1) -> start ->
 *   loop { wait_block -> fill -> submit } -> close
 * Signals the driver needs (interrupt or IO completion) are allocated inside open() and so
 * belong to the audio task.
 *
 * The auxiliary stream (voice clips; LRV_AUX_CHANNEL of the HAL): a driver may also play a
 * second mono stream at a rate of its own (Paula: the two channels the 8-bit main stream
 * leaves free; AHI: a second stream the device mixes). The engine pushes its samples from
 * the main task into a FIFO (audio.c); the audio task opens the driver's second output when
 * the engine asks for it, then keeps two blocks in flight like the main stream's, filling
 * them from the FIFO (silence when it is empty). wait_block reports a finished block of it as
 * AM_VOICE_BLOCK + its index, and AM_AUDIO_CMD when one of `cmd_mask`'s signals came (the
 * engine wants something). */
#ifndef LRVNE_BACKENDS_AMIGA_AUDIO_H
#define LRVNE_BACKENDS_AMIGA_AUDIO_H

#include <exec/types.h>
#include <stdint.h>

struct am_audio_drv {
    const char *name;
    /* Open the output for `rate` Hz, blocks of `frames` mono samples. Returns the rate the
     * hardware really plays (the engine is told, and renders at it), or 0 when the device is
     * unavailable (the program then runs silent). */
    int  (*open)(int rate, int frames);
    void (*close)(void);
    /* Block buffer the task may render into directly (16-bit mono, `frames` samples), or
     * NULL when the driver needs a conversion step (submit_block then converts from the
     * task's scratch buffer). */
    int16_t *(*block_buffer)(int idx);
    /* Queue block idx (its data is in block_buffer(idx) or in `pcm`). */
    void (*submit_block)(int idx, const int16_t *pcm, int frames);
    /* Start playback once both blocks are queued. */
    void (*start)(void);
    /* Block until a block is free to be refilled and return its index (a block of the
     * auxiliary stream: AM_VOICE_BLOCK + index), AM_AUDIO_CMD for a signal of `cmd_mask`, or
     * -1 when one of `quit_mask`'s signals arrived (or the device was taken away). */
    int  (*wait_block)(ULONG quit_mask, ULONG cmd_mask);

    /* The auxiliary output; NULL when the driver has none (the library then mixes the stream
     * into the main one). voice_open: a mono stream at `rate`, blocks of `frames`; returns the
     * rate it plays at, 0 if it cannot (channels taken, no memory). voice_submit queues block
     * idx from 16-bit samples (silence after `frames` real ones up to the block size);
     * voice_start once both are queued. */
    int  (*voice_open)(int rate, int frames);
    void (*voice_submit)(int idx, const int16_t *pcm, int frames);
    void (*voice_start)(void);
    void (*voice_close)(void);
    /* a cut: both blocks silenced in place, the one playing included */
    void (*voice_silence)(void);
    /* Hardware volume, 1..100 percent, which = LRV_VOL_MAIN or LRV_VOL_AUX; returns 1 if the
     * driver applies it (it keeps the value for open/start), 0 if it cannot (the samples are
     * then scaled before they get here). Called from the main task: only stores, or writes a
     * register. NULL = never. */
    int  (*set_volume)(int which, int percent);
};

#define AM_VOICE_BLOCK  2               /* wait_block: auxiliary block 0 or 1 is free */
#define AM_AUDIO_CMD    4               /* wait_block: the engine asked for something */
#define AM_VOICE_LOST   5               /* wait_block: the auxiliary output stopped for good */

/* diagnostics the drivers maintain (read by the main task at close) */
extern volatile ULONG am_audio_stat_ints;      /* device interrupts / request completions */
extern volatile ULONG am_audio_stat_flags;     /* driver-specific bits (paula: 1 = first main
                                                 * block latched, 2 = first auxiliary block,
                                                 * 4 = auxiliary channels stolen) */

extern const struct am_audio_drv am_audio_paula;    /* audio_paula.c: Paula through audio.device */
extern const struct am_audio_drv am_audio_ahi;      /* audio_ahi.c: ahi.device, device interface */

#endif

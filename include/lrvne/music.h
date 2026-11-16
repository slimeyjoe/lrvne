/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The music path, assembled: a music driver in front of a write queue (lrvne/opnq.h), an OPN
 * core (lrvne/opn.h) and the mixer (lrvne/mixer.h), with the HAL's audio stream.
 *
 * The driver is the engine's or an lrvne module's (lrvne/pmd.h). It is given as the queue's
 * sequencer (struct lrv_opnq_seq): clock-driven (run_until, writes with lrv_opnq_write_at) or
 * frame-driven (advance, writes with lrv_opnq_write). What this file does for it is the same
 * for every engine:
 *
 *   - the core: the one lrv_run_cfg.opn_core selects, created before the device is opened,
 *     because a core may insist on a rate of its own (ymfm renders at clock / 72); the device
 *     is then asked for that rate, and when it insists on another the core follows it, or the
 *     light C core takes over at the device's rate;
 *   - no device (audio off, or none there): no core; with idle_rate the driver's time still
 *     passes, a frame's worth of samples at idle_rate rendered into nothing every frame (the
 *     mixer's idle hook), so that the game's waits on the music end as they would with sound;
 *     the voice (the mixer's auxiliary source) keeps its time with it. Without idle_rate the
 *     driver stands still while there is no device;
 *   - offline (a song rendered to a file, struct lrv_app.render_music): the core at the rate
 *     wanted, no device; the caller renders with lrv_mixer_render.
 *
 * Use:
 *     lrv_music_open(&m, hal, &cfg)       the core and the stream; nothing renders yet
 *     ... set the driver up to count in lrv_music_rate(&m), under lrv_mixer_lock(&m.mixer)
 *     lrv_music_start(&m)                 the sources run from now on
 *     ... every call into the driver's state under lrv_mixer_lock / lrv_mixer_unlock
 *     lrv_music_close(&m)
 * The mixer is the engine's to use directly: its volume, its auxiliary source, the pump hook
 * (lrv_mixer_pump with &m.mixer). The structure must not move after lrv_music_open: the mixer
 * and the backend keep pointers into it. */
#ifndef LRVNE_MUSIC_H
#define LRVNE_MUSIC_H

#include <stdint.h>
#include "lrvne/core.h"
#include "lrvne/mixer.h"
#include "lrvne/opnq.h"

#define LRV_MUSIC_OFFLINE 1u        /* flags: render to the caller, never open a device */

struct lrv_music_cfg {
    struct lrv_opnq_seq seq;        /* the driver */
    int core;                       /* lrv_run_cfg.opn_core (lrv_opn_select) */
    int rate;                       /* lrv_run_cfg.audio_rate: the output rate wanted; <= 0 =
                                     * no device (offline: idle_rate is used) */
    uint32_t clock_hz;              /* the chip's master clock, e.g. LRV_OPN_CLOCK_PC98 */
    uint32_t bus_clocks;            /* struct lrv_opnq_cfg's */
    uint32_t queue_size;
    unsigned queue_flags;
    uint32_t idle_rate;             /* the rate the driver counts in without a device; 0 = it
                                     * stands still then */
    uint32_t frame_num, frame_den;  /* frames per second = num / den, e.g. 5642 / 100: what
                                     * one frame's worth is without a device */
    int gain_floor;                 /* struct lrv_mixer_cfg's */
    unsigned flags;                 /* LRV_MUSIC_* */
};

struct lrv_music {
    const struct lrv_hal *hal;
    struct lrv_music_cfg cfg;
    struct lrv_opnq q;
    struct lrv_mixer mixer;
    const char *core_name;          /* the core that renders; NULL without one */
    uint32_t rate;                  /* the rate the driver counts in */
    uint32_t silent_acc;            /* 16.16 samples of the frames passed without a device */
};

/* Creates the core and the queue, initialises the mixer and opens the device (unless
 * offline). Returns LRV_OK, LRV_ERR_NOMEM when the core or the queue cannot be created, or
 * LRV_ERR_RANGE for a configuration the queue refuses. */
int  lrv_music_open(struct lrv_music *m, const struct lrv_hal *hal,
                    const struct lrv_music_cfg *cfg);

/* The rate the driver counts in: the device's (the core's), the wanted rate offline, else
 * idle_rate (0 without it). */
uint32_t lrv_music_rate(const struct lrv_music *m);

/* 1 when a core renders (a device is open, or offline). */
int  lrv_music_has_chip(const struct lrv_music *m);

/* The driver is ready: the mixer runs the sources at lrv_music_rate from now on. */
void lrv_music_start(struct lrv_music *m);

/* Closes the stream (the sources are not called after it), frees the core and the queue. */
void lrv_music_close(struct lrv_music *m);

#endif

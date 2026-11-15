/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The hardware abstraction layer: everything the library and an engine built on it need from
 * the host is one of the function pointers of struct lrv_hal, filled in by a backend
 * (backends/<name>/).
 *
 * Code above the HAL is portable C99 that does no file I/O, starts no threads and reads no
 * clocks. It is single-threaded: every blocking wait ends in frame_wait(), which is where a
 * backend pumps events and paces time. A backend may use interrupts or threads internally as
 * long as the HAL calls stay callable from that single context. The one sanctioned crossing is
 * audio in pull mode (see struct lrv_audio_source).
 *
 * The HAL is one per process, so its functions take no context argument. Optional members are
 * marked; a backend leaves them NULL (or 0). */
#ifndef LRVNE_HAL_H
#define LRVNE_HAL_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/input.h"
#include "lrvne/surface.h"
#include "lrvne/trace.h"

/* ---- video ------------------------------------------------------------------------------ */

/* The display of the emulated machine. */
struct lrv_video_mode {
    int format;                 /* enum lrv_pixfmt of the pages presented */
    int w, h;                   /* pixels */
    int aspect_w, aspect_h;     /* the picture's shape on its monitor, e.g. 4:3 (pixels need
                                 * not be square: 640x400 shown 4:3 has 1:1.2 pixels) */
    uint32_t frame_ns;          /* the frame period in nanoseconds (vertical refresh) */
    int colours;                /* palette entries */
    int rgb_bits;               /* bits per gun of a palette entry */
};

enum lrv_scale_mode {
    LRV_SCALE_INTEGER = 0,      /* the same integer factor on both axes, square host pixels */
    LRV_SCALE_INTEGER_ASPECT,   /* independent integer factors approximating the aspect */
    LRV_SCALE_FIT,              /* fill the window keeping the aspect */
    LRV_SCALE_STRETCH           /* fill the whole window */
};

struct lrv_video_cfg {
    struct lrv_video_mode mode;
    const char *title;          /* window title, UTF-8 */
    int fullscreen;             /* start in fullscreen */
    int scale_mode;             /* enum lrv_scale_mode, where the backend scales */
    uint32_t host_mode;         /* a backend's own display mode id, 0 = its default */
};

/* One presentation. A frame describes itself: what it shows can be converted, hashed or saved
 * without the video mode at hand (the page has its own format and size, the palette its own). */
struct lrv_frame {
    const struct lrv_surface *page; /* the displayed page */
    const struct lrv_rect *dirty;   /* what changed since the previous present; NULL = all */
    int page_switched;              /* a different page is displayed now: upload everything */
    int scroll_line;                /* the page line shown at the top (hardware scroll) */
    const uint8_t *rgb;             /* palette: colours x {R,G,B}, 0 .. 2^rgb_bits - 1, as the
                                     * DAC outputs them */
    int colours;                    /* palette entries: the video mode's */
    int rgb_bits;                   /* bits per gun: the video mode's */
    int display_on;                 /* 0: the display shows black */
};

/* ---- packs, audio --------------------------------------------------------------------- */

/* Which pack pack_open is asked for. The voice pack is optional: a backend returns NULL when
 * it has none or the player turned the voice off. */
enum lrv_pack_id { LRV_PACK_MAIN = 0, LRV_PACK_VOICE = 1, LRV_PACK_COUNT };

/* Where a pulling backend gets its samples (audio_open): mono 16-bit PCM in blocks, asked for
 * from the backend's audio thread or interrupt whenever the device needs them, the way a
 * music driver ran from a timer interrupt.
 *
 * The locking rule of pull mode: both callbacks take audio_lock themselves for the block (the
 * mixer of lrvne/mixer.h does). A backend therefore calls them WITHOUT holding the lock, and
 * the lock need not be recursive. From the main context every call into sound state is
 * bracketed with audio_lock / audio_unlock, never nested. */
typedef void lrv_audio_fn(void *ctx, int16_t *pcm, int frames);

struct lrv_audio_source {
    lrv_audio_fn *render;   /* the next `frames` samples into pcm */
    /* optional (NULL: use render): the same span of time for a backend that has fallen behind
     * and drops a block rather than fall further. Time passes exactly as in render (music and
     * clips are `frames` samples further on) but whatever is expensive to synthesise is not
     * run; pcm receives what was cheap to have, silence at the least, and the backend may
     * play or discard it. */
    lrv_audio_fn *skip;
    void *ctx;
};

/* How a backend outputs the auxiliary PCM stream (voice clips). */
enum lrv_aux_mode {
    LRV_AUX_NONE = 0,       /* no auxiliary stream */
    LRV_AUX_MIX,            /* the library mixes it into the main stream it renders */
    LRV_AUX_CHANNEL         /* the backend plays it on its own output (aux_* below) */
};

enum lrv_volume { LRV_VOL_MAIN = 0, LRV_VOL_AUX };

/* ---- memory, log ---------------------------------------------------------------------- */

#define LRV_MEM_ANY  0u
#define LRV_MEM_CHIP 1u     /* memory the display or sound hardware can reach (Amiga chip RAM) */

enum lrv_log_level { LRV_LOG_DEBUG, LRV_LOG_INFO, LRV_LOG_WARN, LRV_LOG_ERROR };

/* ---- the HAL -------------------------------------------------------------------------- */

struct lrv_hal {
    /* video. The pages belong to the caller; the backend only reads them. */
    int  (*video_open)(const struct lrv_video_cfg *cfg);        /* 0 or enum lrv_err */
    void (*video_close)(void);
    void (*video_present)(const struct lrv_frame *frame);

    /* time: blocks until the next frame of the video mode (mode.frame_ns). */
    void (*frame_wait)(void);

    /* input */
    void (*input_poll)(struct lrv_input *in);
    /* drops the type-ahead (key presses and button edges) but keeps the LRV_EV_STICKY events
     * for the next poll: they must never be lost to a flush */
    void (*input_flush)(void);
    /* optional: move the host pointer to (x, y) in video mode pixels */
    void (*mouse_warp)(int x, int y);

    /* audio: mono 16-bit PCM. audio_open returns the rate the backend accepted (it may differ
     * from the request) or 0 when there is no audio; it copies *src. Two delivery models:
     *  push (audio_lock == NULL): the caller renders from its frame and uses audio_space /
     *    audio_push; src is not used. The backend must buffer generously.
     *  pull (audio_lock != NULL): the backend calls src->render (or src->skip) from its audio
     *    thread or interrupt, from audio_open's return until audio_close returns, never
     *    holding the lock (struct lrv_audio_source has the rule). */
    int  (*audio_open)(int rate, const struct lrv_audio_source *src);
    void (*audio_close)(void);
    int  (*audio_space)(void);                          /* frames the sink can take now */
    void (*audio_push)(const int16_t *pcm, int frames);
    void (*audio_lock)(void);                           /* pull mode only, else NULL */
    void (*audio_unlock)(void);

    /* auxiliary PCM stream (voice). aux_mode says which model the backend implements; the
     * functions are for LRV_AUX_CHANNEL: mono 16-bit PCM pushed from the caller's frame, the
     * backend mixes and resamples as it likes.
     *   aux_open(rate)  -> the accepted rate (0 = cannot)
     *   aux_space()     -> frames that can be pushed now
     *   aux_push(p, n)  -> n frames; n = 0 ends a clip: play out anything held back
     *   aux_queued()    -> frames pushed but not yet played (the clip has ended at 0)
     *   aux_flush()     -> drop everything queued at once (a clip is cut)
     *   aux_close()
     * LRV_AUX_CHANNEL requires aux_open, aux_space, aux_push and aux_queued (lrvne/voice.h
     * refuses to play without them); aux_close and aux_flush are optional, and without
     * aux_flush a clip that is cut plays out what is already queued. With LRV_AUX_MIX and
     * LRV_AUX_NONE all of them stay NULL. */
    int  aux_mode;                                      /* enum lrv_aux_mode */
    int  aux_lead_ms;       /* CHANNEL: how much the backend wants queued ahead, 0 = default */
    int  (*aux_open)(int rate);
    void (*aux_close)(void);
    int  (*aux_space)(void);
    void (*aux_push)(const int16_t *pcm, int frames);
    int  (*aux_queued)(void);
    void (*aux_flush)(void);
    /* optional: the backend applies a volume (enum lrv_volume, percent 1..100) in its output.
     * Returns 1 if it does, 0 if the samples are to be scaled before they reach it. For
     * LRV_VOL_AUX a backend can only take it on a stream of its own: with LRV_AUX_MIX (and
     * LRV_AUX_NONE) it returns 0, the clips being scaled before they are mixed. */
    int  (*set_volume)(int which, int percent);

    /* storage: small named records (saves, settings). storage_open selects the game's own
     * store by its id (a short file-name-safe word, lrvne/profile.h) and is called once,
     * before the first read or write. 0 on success; reading a record that does not exist
     * fails with LRV_ERR_NOTFOUND. */
    int  (*storage_open)(const char *game_id);
    int  (*storage_read)(const char *key, void *buf, size_t cap, size_t *len);
    int  (*storage_write)(const char *key, const void *buf, size_t len);

    /* packs: an opaque source ranges are read from (lrvne/pack.h). pack_open returns a handle
     * and the byte size of the source (a pack file, or an executable with a pack appended),
     * or NULL. */
    void *(*pack_open)(int which, uint32_t *size);
    size_t (*pack_read)(void *h, uint32_t off, void *dst, size_t n);
    void (*pack_close)(void *h);

    /* memory and diagnostics */
    void *(*mem_alloc)(size_t n, unsigned flags);       /* LRV_MEM_*; not zeroed */
    void (*mem_free)(void *p, size_t n);
    void (*log)(int level, const char *msg);
    void (*fatal)(const char *msg);                     /* show the message and exit */

    /* optional, test automation: what the engine is doing (lrvne/trace.h) */
    void (*trace)(const struct lrv_trace_event *ev);
};

#endif

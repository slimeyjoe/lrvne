/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Amiga family's backend - shared declarations of the core part (README.md "Layout").
 *
 * Files: main.c (lrv_app_main: startup, options, the HAL table, memory, log), paths.c (where
 * the packs and the saves are, pack and storage I/O on AmigaDOS), video.c (screen, screen
 * buffers, present), video_win.c (the window on a public screen), input.c (window, IDCMP,
 * raw keys, pointer, screen front/back policy), timer.c (the frame clock), frame.c +
 * frameos.c (the once-per-frame OS call), audio.c (audio task, pull-mode HAL audio, the
 * auxiliary stream) with the output drivers behind the interface in audio.h (audio_ahi.c
 * here, chipset/audio_paula.c), snap.c (test snapshots). What differs by system is behind
 * amsys.h (exec/ or warpos/), what differs by flavour behind flavour.h (os3/, warpos/,
 * aros/), what differs by compiler in amcompat.h.
 *
 * Nothing here knows an engine or a game: names come from struct lrv_app, the video mode
 * from the engine's video_open, the game's id from storage_open. */
#ifndef LRVNE_BACKENDS_AMIGA_H
#define LRVNE_BACKENDS_AMIGA_H

#include "os_begin.h"
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include "os_end.h"
#include <stddef.h>
#include <stdint.h>
#include "amcompat.h"
#include "lrvne/app.h"
#include "lrvne/core.h"
#include "lrvne/hal.h"

/* The backend's own options (main.c has the table: the shell's template and the icon's tool
 * types are generated from it and from the tables every backend shares, backends/common/
 * args.h). */
struct am_opts {
    ULONG mode_id;          /* MODE=0x...: display mode id (default: the flavour's) */
    const char *audio;      /* AUDIO=paula|ahi|none (default: the flavour's first driver) */
    LONG audiobuf;          /* AUDIOBUF=frames per block (default: 93 ms at the rate) */
    LONG audio_cpu;         /* AUDIOCPU=percent of real time the renderer may use (default 75) */
    LONG ahi_unit;          /* AHIUNIT=n (default 0) */
    int paula14;            /* PAULA14: 14-bit output on four channels */
    int voicemix;           /* VOICEMIX: the library mixes the voice into the main stream */
    int quiet;              /* QUIET: no console output */
    int fast;               /* FAST: no frame pacing, the engine runs as fast as it can (tests) */
    int present_test;       /* PRESENTTEST: run the synthetic presentation test (ptest.h) */
    const char *present_mode; /* PRESENTTESTMODE=machine: the same in that mode, no engine */
    const char *snapdir;    /* SNAPDIR=dir: where snapshot requests go (default PROGDIR:) */
    const char *preload;    /* PRELOAD=auto|yes|no: the whole pack into fast RAM (default auto) */
    const char *upload;     /* UPLOAD=name: the graphics card upload (video.c, upload.h) */
    int scale;              /* SCALE=1|2|3|auto (0 = auto): page pixels shown n x n (default 1) */
    int aspect;             /* ASPECT: show the page in the video mode's aspect (rows repeated) */
    int window;             /* WINDOW: in a window on a public screen (video_win.c) */
    const char *pubscreen;  /* PUBSCREEN=name: that screen (default: the default public screen) */
    int help;               /* HELP: list the options and exit */
};
extern struct am_opts am_opts;
extern const struct lrv_app *am_app;        /* the engine (main.c) */
extern struct Task *am_main_task;
extern int am_from_wb;
extern int am_audio_rate;                   /* the output rate asked for, 0 = no audio */
extern int am_no_voice;                     /* no voice pack is to be opened */
/* names of the program's tasks and interrupts: "<app name> audio", "<app name> voice" */
extern char am_name_audio[48], am_name_aux[48];

/* main.c */
void am_log(const char *fmt, ...);          /* console output (CLI start, main task only) */
int  am_strieq(const char *a, const char *b); /* case-insensitive string equality */

/* paths.c: the packs and the saves (README.md "Packs and saves") */
struct am_paths_cfg {
    const char *pak;        /* PAK= or NULL */
    const char *voice_pak;  /* VOICEPAK= or NULL */
    int no_voice;           /* NOVOICE */
    const char *storage;    /* STORAGE= or NULL */
};
void   am_paths_init(const struct am_paths_cfg *cfg);
void  *am_pack_open(int which, uint32_t *size);
size_t am_pack_read(void *h, uint32_t off, void *dst, size_t n);
void   am_pack_close(void *h);
int    am_storage_open(const char *game_id);
int    am_storage_read(const char *key, void *buf, size_t cap, size_t *len);
int    am_storage_write(const char *key, const void *buf, size_t len);

/* video.c */
int  am_video_open(const struct lrv_video_cfg *cfg);
void am_video_close(void);
void am_video_present(const struct lrv_frame *frame);
struct Screen *am_video_screen(void);
/* A position inside the input window to a pixel of the video mode, clamped to the page.
 * Returns 0 when there is no display. */
int  am_video_to_page(int wx, int wy, int *x, int *y);
/* How many display pixels one pixel of the video mode is wide (1 without a display). */
int  am_video_scale_x(void);

/* What a snapshot reader needs to find the displayed image in memory (snap.c). */
struct am_snapinfo {
    int shown;              /* index of the displayed screen buffer */
    int standard;           /* BMF_STANDARD: planar chip RAM; 0 = graphics card bitmap */
    ULONG bpr, rows, depth;
    void *planes[8];
    ULONG mode;             /* display mode id of the screen */
    struct BitMap *bm;
    int pen_base;           /* pen of colour 0 (16 on a graphics card screen) */
    int w, h;               /* the page: the video mode's size */
    int dx, dy;             /* the page's top left in the bitmap (a screen larger than it) */
    int sx, dst_h;          /* SCALE/ASPECT (upload.h am_geom); sx 0 = the page as it is */
    int window;             /* in a window (video_win.c): dx,dy = its inside on the screen */
    int pens[16];           /* the window's pens of the page's colours */
    int mouse_x, mouse_y;   /* the pointer on the screen (visible in a window) */
};
void am_video_snapinfo(struct am_snapinfo *si);

/* snap.c: tells a test rig on the host to take a snapshot of the screen now and waits until
 * it has. hash = lrv_frame_hash32 of what must be shown (have_hash 0: unknown). */
void am_snap(const char *name, uint32_t tick, uint32_t hash, int have_hash);

/* video_win.c: the page in a window on a public screen (WINDOW). am_win_open: 0 = open,
 * 1 = the screen cannot take it (the caller opens its own screen), -1 = failure */
int  am_win_open(const struct lrv_video_cfg *cfg);
void am_win_close(void);
void am_win_present(const struct lrv_frame *frame);
void am_win_snapinfo(struct am_snapinfo *si);
int  am_win_to_page(int wx, int wy, int *x, int *y);
void am_win_refresh(void);      /* input.c: IDCMP_REFRESHWINDOW */
void am_win_changed(void);      /* input.c: IDCMP_CHANGEWINDOW */
void am_win_cycle(void);        /* input.c: F10, the next size */

/* input.c */
int  am_input_open(struct Screen *scr);
int  am_input_attach(struct Window *win);   /* the window of video_win.c */
void am_input_close(void);
void am_input_poll(struct lrv_input *in);
void am_input_flush(void);

/* timer.c */
int  am_timer_open(void);
void am_timer_close(void);
void am_timer_set_period(uint32_t frame_ns);    /* the video mode's frame (video_open) */
uint32_t am_timer_period_us(void);
void am_frame_wait(ULONG ops);           /* frameos.h AM_F_*: one OS call per frame */
void am_frame_resync(void);              /* after a deliberate pause (snap.c) */
uint32_t am_ticks(void);                 /* frame periods of real time since the start */
/* Stall profile: time spent in pack reads and presents (main task) and in rendering
 * (audio task) since the frame clock was last on time; one log line per stall of more than
 * ten frames says where the time went and which pack bytes were read. All main-task times are
 * wall time: the audio task (higher priority) runs inside them, so its share is listed apart. */
/* The profile clock (the system part's, amsys.h am_prof_clock_init): E-clock ticks (low 32
 * bits); on WarpOS PPC time base ticks / 16 (about 1 MHz, read without a 68k call). Any
 * task. */
ULONG am_prof_now(void);
extern struct Device *TimerBase;            /* timer.c, once timer.device is open */
ULONG am_prof_ms(ULONG eticks);
ULONG am_prof_freq(void);                   /* profile ticks per second */
void  am_prof_pack(ULONG t0, ULONG t_seek, ULONG off, ULONG n); /* t_seek: after the Seek */
void  am_prof_present(ULONG t0, int full);
ULONG am_prof_present_ms(ULONG *calls);    /* presents of the whole run: total ms, count */
extern volatile ULONG am_prof_audio;        /* profile ticks the audio task spent rendering */

/* main.c: stack high-water mark. am_stack_paint fills the unused part of the calling task's
 * stack with a pattern; am_stack_used (same task or later) finds the deepest word that is no
 * longer the pattern. size 0 = the stack bounds did not contain the current stack pointer. */
struct am_stack { ULONG *lower, *upper; };
void  am_stack_paint(struct am_stack *st);
ULONG am_stack_used(const struct am_stack *st);
ULONG am_stack_size(const struct am_stack *st);
/* audio.c: the audio task's stack, measured by the task itself as it ends (its stack is gone
 * with it) */
extern ULONG am_audio_stack_used, am_audio_stack_total;

/* audio.c: the HAL's audio functions */
void am_audio_init(void);                   /* before the engine runs: the lock must exist early */
void am_audio_exit(void);                   /* frees the lock (after the engine returned) */
int  am_audio_open(int rate, const struct lrv_audio_source *src);
void am_audio_close(void);
int  am_audio_aux_mode(void);               /* enum lrv_aux_mode the options give */
int  am_audio_block_frames(int rate);       /* frames of one block at `rate` (AUDIOBUF) */
int  am_audio_set_volume(int which, int percent);
int  am_aux_open(int rate);                 /* main task */
void am_aux_close(void);
int  am_aux_space(void);
void am_aux_push(const int16_t *pcm, int frames);
int  am_aux_queued(void);
void am_aux_flush(void);
void am_audio_lock(void);
void am_audio_unlock(void);

#endif

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Control mode: the test automation protocol of the hosted backends (--control).
 *
 * A line protocol on stdin and stdout. The backend writes what the engine reports (its trace
 * events, lrvne/trace.h) as JSON lines and stops at well-defined points: the engine waits for
 * the player's input, an event with a breakpoint was reported, a budget of ticks ran out. At a
 * stop it reads commands until one resumes the engine. A test driver therefore never sleeps
 * and never guesses: it reads events up to a stop, decides, and answers.
 *
 * The protocol knows no engine: the names of an engine's trace kinds and which of its waits
 * wait for input come from struct lrv_app, keys are named as in lrvne/input.h, and frames
 * have the size of the video mode. control.c documents the lines and the commands.
 *
 * A backend calls, in this order of a frame: lrv_ctl_present from video_present,
 * lrv_ctl_tick from frame_wait, lrv_ctl_input from input_poll.
 *
 * When the library is built without the protocol (LRVNE_CONTROL=0) the functions are empty
 * and lrv_ctl_available() is 0, so a backend needs no conditional code. */
#ifndef LRVNE_BACKENDS_CONTROL_H
#define LRVNE_BACKENDS_CONTROL_H

#include <stdint.h>
#include "lrvne/app.h"
#include "lrvne/hal.h"

/* What a backend that really renders put on the screen for the last presented frame, so that
 * a driver can compare the rendered pixels with the engine's page (the `readback` command).
 * rgb holds one sample per pixel of the video mode, taken at the centre of the destination
 * block of that pixel (mode.w x mode.h x 3 bytes, row-major). nonuniform counts the
 * destination pixels inside the rectangle that differ from their source pixel's sample (zero
 * for exact integer scaling), border_lit the non-black pixels outside it. */
struct lrv_ctl_readback {
    int win_w, win_h;                   /* drawable size in pixels */
    int dst_x, dst_y, dst_w, dst_h;     /* where the picture went */
    int scale_mode;                     /* enum lrv_scale_mode */
    int linear;                         /* 1 = filtered scaling: only flat areas are exact */
    long nonuniform;
    long border_lit;
    uint8_t *rgb;                       /* provided by the caller of the function */
};

#ifdef LRV_HAVE_CONTROL

static inline int lrv_ctl_available(void) { return 1; }

/* Starts the protocol for an engine. */
void lrv_ctl_init(const struct lrv_app *app);
/* video_open: the mode of the frames to come. */
void lrv_ctl_video(const struct lrv_video_mode *mode);
/* hal->trace. */
void lrv_ctl_trace(const struct lrv_trace_event *ev);
/* video_present: keeps a copy of the frame, and ends the run (exit code 1) when the page
 * changed outside the frame's dirty rectangle. */
void lrv_ctl_present(const struct lrv_frame *frame);
/* frame_wait, after the tick counter advanced; blocks reading commands at a stop. */
void lrv_ctl_tick(uint32_t tick);
/* input_poll: what the driver set (levels and pending presses); the presses are consumed. */
void lrv_ctl_input(struct lrv_input *in);
void lrv_ctl_input_flush(void);
/* The backend's readback function, when it has one. Returns 0 on success. */
void lrv_ctl_set_readback(int (*fn)(struct lrv_ctl_readback *rb));
/* The engine finished (its exit code) or failed: writes the exit line and ends the process. */
void lrv_ctl_exit(int code, const char *msg);

#else

static inline int lrv_ctl_available(void) { return 0; }
static inline void lrv_ctl_init(const struct lrv_app *app) { (void)app; }
static inline void lrv_ctl_video(const struct lrv_video_mode *mode) { (void)mode; }
static inline void lrv_ctl_trace(const struct lrv_trace_event *ev) { (void)ev; }
static inline void lrv_ctl_present(const struct lrv_frame *frame) { (void)frame; }
static inline void lrv_ctl_tick(uint32_t tick) { (void)tick; }
static inline void lrv_ctl_input(struct lrv_input *in) { (void)in; }
static inline void lrv_ctl_input_flush(void) {}
static inline void lrv_ctl_set_readback(int (*fn)(struct lrv_ctl_readback *rb)) { (void)fn; }
static inline void lrv_ctl_exit(int code, const char *msg) { (void)code; (void)msg; }

#endif

#endif

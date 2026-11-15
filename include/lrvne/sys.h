/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The system layer: the single blocking point of an engine (the frame), the input model built
 * on the HAL's polls, and trace events.
 *
 * One frame, lrv_sys_frame_wait(), is:
 *     pump hooks        feed audio, step animations that draw into the page
 *     present           the callback set with lrv_sys_set_present (lrvne/gfx.h has one)
 *     hal->frame_wait   block until the next vertical refresh of the video mode
 *     frame count + 1
 *     tick hooks        what the original machine did in its refresh interrupt
 *     input poll
 * Once a quit or reset was requested, a frame no longer blocks, presents or polls: the count
 * advances and the tick hooks run, so the loops of the engine unwind at once. */
#ifndef LRVNE_SYS_H
#define LRVNE_SYS_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/core.h"
#include "lrvne/hal.h"

#define LRV_HOOKS_MAX 8

enum lrv_hook_stage {
    LRV_HOOK_PUMP = 0,      /* before presenting */
    LRV_HOOK_TICK,          /* after the frame count advanced */
    LRV_HOOK_STAGES
};

typedef void lrv_hook_fn(void *ctx);

struct lrv_hook {
    lrv_hook_fn *fn;
    void *ctx;
};

struct lrv_sys {
    const struct lrv_hal *hal;

    /* input */
    struct lrv_input in;                    /* the last poll: levels */
    struct lrv_keypress kbuf[LRV_KEYQ];     /* key presses not yet consumed */
    int khead, kcount;
    unsigned edges;                         /* LRV_EV_MOUSE_* not yet consumed */
    int mouse_x, mouse_y;                   /* the pointer the engine works with */
    int ptr_w, ptr_h;                       /* its bounds: the video mode's size */
    int quit_requested, emergency_quit, reset_requested;

    /* time */
    uint32_t frame;                         /* frames since lrv_sys_init */
    uint32_t cpu;                           /* work spent since the last frame (lrv_sys_cpu) */
    uint32_t cpu_per_frame;                 /* the reference machine's speed; 0 = not modelled */
    struct lrv_hook hooks[LRV_HOOK_STAGES][LRV_HOOKS_MAX];
    int nhooks[LRV_HOOK_STAGES];
    lrv_hook_fn *present;
    void *present_ctx;

    /* trace context: stamped on every event */
    const char *script_name;
    uint32_t script_pos;                    /* ~0u = none */
};

/* Start: empty queues, the pointer in the middle of a ptr_w x ptr_h screen. */
void lrv_sys_init(struct lrv_sys *s, const struct lrv_hal *hal, int ptr_w, int ptr_h);

/* ---- frames --------------------------------------------------------------------------- */

void lrv_sys_set_present(struct lrv_sys *s, lrv_hook_fn *present, void *ctx);
/* Adds a hook (they run in the order added). Returns LRV_ERR_RANGE when the stage is full. */
int  lrv_sys_add_hook(struct lrv_sys *s, int stage, lrv_hook_fn *fn, void *ctx);
void lrv_sys_frame_wait(struct lrv_sys *s);
/* Presents now, outside the frame (before a long computation). */
void lrv_sys_present_now(struct lrv_sys *s);
/* Time of busy loops the original ran without waiting: n units of work of the reference
 * machine (cpu_per_frame per frame). A frame passes whenever a frame's worth is spent;
 * lrv_sys_frame_wait ends the frame's budget. Nothing happens when the speed is not set. */
void lrv_sys_cpu(struct lrv_sys *s, uint32_t n);

/* ---- quit and reset ------------------------------------------------------------------- */

/* The player asked to quit or to reset: the engine unwinds to its top. */
int  lrv_sys_quitting(const struct lrv_sys *s);
/* ... and it is a reset, not a quit. */
int  lrv_sys_resetting(const struct lrv_sys *s);
/* Asks for a quit from inside the engine (its own "quit" menu entry, a tool mode that is done):
 * the same as the host closing the window. */
void lrv_sys_request_quit(struct lrv_sys *s);
/* The reset was done: forget it and the type-ahead. */
void lrv_sys_reset_done(struct lrv_sys *s);

/* ---- input ---------------------------------------------------------------------------- */

int  lrv_sys_key_sense(const struct lrv_sys *s);            /* a key press is waiting */
/* Takes the oldest key press. Returns 0 when there is none. */
int  lrv_sys_key_read(struct lrv_sys *s, struct lrv_keypress *k);
void lrv_sys_key_flush(struct lrv_sys *s);                  /* also drops pending button edges */
int  lrv_sys_key_down(const struct lrv_sys *s, int key);
unsigned lrv_sys_mods(const struct lrv_sys *s);             /* LRV_MOD_* now */
unsigned lrv_sys_mouse_buttons(const struct lrv_sys *s);
/* Consumes press edges (LRV_EV_MOUSE_L / _R): 1 if any of `ev` happened since last consumed. */
int  lrv_sys_mouse_pressed(struct lrv_sys *s, unsigned ev);
/* Moves the pointer (the host's too, where the backend can). */
void lrv_sys_mouse_set(struct lrv_sys *s, int x, int y);

/* ---- trace ---------------------------------------------------------------------------- */

#define LRV_TRACING(s) ((s)->hal->trace != 0)

void lrv_sys_trace(struct lrv_sys *s, int kind, const char *name, const uint8_t *text,
                   int a0, int a1, int a2, int a3);
void lrv_sys_trace_ints(struct lrv_sys *s, int kind, const char *name, const int *ints, int n,
                        int a0);
void lrv_sys_trace_items(struct lrv_sys *s, int kind, const uint8_t *const *items, int n,
                         int a0);

#endif

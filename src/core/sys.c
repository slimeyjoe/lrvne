/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The system layer (lrvne/sys.h): frames, the input model and trace events. */
#include <string.h>
#include "lrvne/sys.h"

void lrv_sys_init(struct lrv_sys *s, const struct lrv_hal *hal, int ptr_w, int ptr_h)
{
    memset(s, 0, sizeof *s);
    s->hal = hal;
    s->ptr_w = ptr_w;
    s->ptr_h = ptr_h;
    s->mouse_x = ptr_w / 2;
    s->mouse_y = ptr_h / 2;
    s->script_pos = ~0u;
}

/* ---- input ----------------------------------------------------------------------------- */

static void kbuf_push(struct lrv_sys *s, const struct lrv_keypress *k)
{
    if (s->kcount >= LRV_KEYQ)
        return;                         /* full: dropped, as a keyboard buffer drops it */
    s->kbuf[(s->khead + s->kcount) % LRV_KEYQ] = *k;
    s->kcount++;
}

static void clamp_pointer(struct lrv_sys *s)
{
    if (s->mouse_x > s->ptr_w - 1) s->mouse_x = s->ptr_w - 1;
    if (s->mouse_y > s->ptr_h - 1) s->mouse_y = s->ptr_h - 1;
    if (s->mouse_x < 0) s->mouse_x = 0;
    if (s->mouse_y < 0) s->mouse_y = 0;
}

static void poll_input(struct lrv_sys *s)
{
    struct lrv_input in;
    int i;
    memset(&in, 0, sizeof in);
    s->hal->input_poll(&in);
    if (in.nkeys > LRV_KEYQ)
        in.nkeys = LRV_KEYQ;
    /* a key pressed and released between two polls still holds its bit for this poll: code
     * that only reads the key-down map must see it, as it would on the machine, where a press
     * lasts many polls */
    for (i = 0; i < in.nkeys; i++) {
        kbuf_push(s, &in.keys[i]);
        lrv_key_set(&in, in.keys[i].key, 1);
    }
    s->edges |= in.events & (LRV_EV_MOUSE_L | LRV_EV_MOUSE_R);
    if (in.events & LRV_EV_QUIT)
        s->quit_requested = 1;
    if (in.events & LRV_EV_EMERGENCY)
        s->emergency_quit = 1;
    if (in.events & LRV_EV_RESET)
        s->reset_requested = 1;
    /* the pointer: an absolute host position wins when it moved, else relative motion */
    if (in.mouse_abs) {
        if (!s->in.mouse_abs || in.mouse_x != s->in.mouse_x || in.mouse_y != s->in.mouse_y) {
            s->mouse_x = in.mouse_x;
            s->mouse_y = in.mouse_y;
        }
        if (s->host_known) {
            s->motion_x += in.mouse_x - s->host_x;
            s->motion_y += in.mouse_y - s->host_y;
        }
        s->host_x = in.mouse_x;
        s->host_y = in.mouse_y;
        s->host_known = 1;
    } else {
        s->mouse_x += in.mouse_dx;
        s->mouse_y += in.mouse_dy;
        s->motion_x += in.mouse_dx;
        s->motion_y += in.mouse_dy;
        s->host_known = 0;
    }
    clamp_pointer(s);
    s->in = in;
    /* a click shorter than a poll still holds its button down for this one poll */
    if (in.events & LRV_EV_MOUSE_L)
        s->in.buttons |= LRV_BTN_L;
    if (in.events & LRV_EV_MOUSE_R)
        s->in.buttons |= LRV_BTN_R;
}

int lrv_sys_quitting(const struct lrv_sys *s)
{
    return s->quit_requested || s->emergency_quit || s->reset_requested;
}

int lrv_sys_resetting(const struct lrv_sys *s)
{
    return s->reset_requested && !s->quit_requested && !s->emergency_quit;
}

void lrv_sys_request_quit(struct lrv_sys *s)
{
    s->quit_requested = 1;
}

void lrv_sys_reset_done(struct lrv_sys *s)
{
    s->reset_requested = 0;
    lrv_sys_key_flush(s);
}

int lrv_sys_key_sense(const struct lrv_sys *s)
{
    return s->kcount > 0;
}

int lrv_sys_key_read(struct lrv_sys *s, struct lrv_keypress *k)
{
    if (!s->kcount)
        return 0;
    *k = s->kbuf[s->khead];
    s->khead = (s->khead + 1) % LRV_KEYQ;
    s->kcount--;
    return 1;
}

void lrv_sys_key_flush(struct lrv_sys *s)
{
    s->hal->input_flush();
    s->khead = s->kcount = 0;
    s->edges = 0;
    /* the input as it is now: a quit the flush kept for the next poll counts at once, not a
     * frame later, and the levels are those after the flush */
    if (!lrv_sys_quitting(s))
        poll_input(s);
}

int lrv_sys_key_down(const struct lrv_sys *s, int key)
{
    return lrv_key_down(&s->in, key);
}

unsigned lrv_sys_mods(const struct lrv_sys *s)
{
    return s->in.mods;
}

unsigned lrv_sys_mouse_buttons(const struct lrv_sys *s)
{
    return s->in.buttons;
}

int lrv_sys_mouse_pressed(struct lrv_sys *s, unsigned ev)
{
    if (s->edges & ev) {
        s->edges &= ~ev;
        return 1;
    }
    return 0;
}

void lrv_sys_mouse_set(struct lrv_sys *s, int x, int y)
{
    s->mouse_x = x;
    s->mouse_y = y;
    clamp_pointer(s);
    if (s->hal->mouse_warp) {
        s->hal->mouse_warp(s->mouse_x, s->mouse_y);
        /* the host pointer jumps there: not the player's motion */
        s->host_x = s->mouse_x;
        s->host_y = s->mouse_y;
    }
}

/* ---- frames ---------------------------------------------------------------------------- */

void lrv_sys_set_present(struct lrv_sys *s, lrv_hook_fn *present, void *ctx)
{
    s->present = present;
    s->present_ctx = ctx;
}

int lrv_sys_add_hook(struct lrv_sys *s, int stage, lrv_hook_fn *fn, void *ctx)
{
    struct lrv_hook *h;
    if (stage < 0 || stage >= LRV_HOOK_STAGES || s->nhooks[stage] >= LRV_HOOKS_MAX)
        return LRV_ERR_RANGE;
    h = &s->hooks[stage][s->nhooks[stage]++];
    h->fn = fn;
    h->ctx = ctx;
    return LRV_OK;
}

/* closes the gaps hooks removed during a run left */
static void compact_hooks(struct lrv_sys *s, int stage)
{
    struct lrv_hook *h = s->hooks[stage];
    int i, n = 0;
    for (i = 0; i < s->nhooks[stage]; i++)
        if (h[i].fn)
            h[n++] = h[i];
    s->nhooks[stage] = n;
}

int lrv_sys_remove_hook(struct lrv_sys *s, int stage, lrv_hook_fn *fn, void *ctx)
{
    int i;
    if (stage < 0 || stage >= LRV_HOOK_STAGES || !fn)
        return LRV_ERR_NOTFOUND;
    for (i = 0; i < s->nhooks[stage]; i++)
        if (s->hooks[stage][i].fn == fn && s->hooks[stage][i].ctx == ctx) {
            s->hooks[stage][i].fn = 0;
            if (!s->hook_depth[stage])
                compact_hooks(s, stage);
            return LRV_OK;
        }
    return LRV_ERR_NOTFOUND;
}

static void run_hooks(struct lrv_sys *s, int stage)
{
    int i, n = s->nhooks[stage];            /* the hooks added during the run wait */
    s->hook_depth[stage]++;
    for (i = 0; i < n; i++)
        if (s->hooks[stage][i].fn)          /* not removed meanwhile */
            s->hooks[stage][i].fn(s->hooks[stage][i].ctx);
    if (!--s->hook_depth[stage])
        compact_hooks(s, stage);
}

void lrv_sys_present_now(struct lrv_sys *s)
{
    if (s->present)
        s->present(s->present_ctx);
}

void lrv_sys_frame_wait(struct lrv_sys *s)
{
    s->cpu = 0;
    if (!lrv_sys_quitting(s)) {
        run_hooks(s, LRV_HOOK_PUMP);
        lrv_sys_present_now(s);
        s->hal->frame_wait();
    }
    s->frame++;
    run_hooks(s, LRV_HOOK_TICK);
    if (!lrv_sys_quitting(s))
        poll_input(s);
}

void lrv_sys_cpu(struct lrv_sys *s, uint32_t n)
{
    if (!s->cpu_per_frame)
        return;
    s->cpu += n;
    while (s->cpu >= s->cpu_per_frame && !lrv_sys_quitting(s)) {
        uint32_t left = s->cpu - s->cpu_per_frame;
        lrv_sys_frame_wait(s);
        s->cpu = left;
    }
}

/* ---- trace ----------------------------------------------------------------------------- */

int lrv_sys_trace_begin(const struct lrv_sys *s, struct lrv_trace_event *ev, int kind)
{
    if (!s->hal->trace)
        return 0;
    memset(ev, 0, sizeof *ev);
    ev->kind = kind;
    ev->tick = s->frame;
    ev->pos = s->script_pos;
    ev->script = s->script_name;
    return 1;
}

void lrv_sys_trace_send(struct lrv_sys *s, const struct lrv_trace_event *ev)
{
    if (s->hal->trace)
        s->hal->trace(ev);
}

void lrv_sys_trace(struct lrv_sys *s, int kind, const char *name, const uint8_t *text,
                   int a0, int a1, int a2, int a3)
{
    struct lrv_trace_event ev;
    if (!lrv_sys_trace_begin(s, &ev, kind))
        return;
    ev.name = name;
    ev.text = text;
    ev.a0 = a0; ev.a1 = a1; ev.a2 = a2; ev.a3 = a3;
    lrv_sys_trace_send(s, &ev);
}

void lrv_sys_trace_ints(struct lrv_sys *s, int kind, const char *name, const int *ints, int n,
                        int a0)
{
    struct lrv_trace_event ev;
    if (!lrv_sys_trace_begin(s, &ev, kind))
        return;
    ev.name = name;
    ev.ints = ints;
    ev.nints = n;
    ev.a0 = a0;
    lrv_sys_trace_send(s, &ev);
}

void lrv_sys_trace_items(struct lrv_sys *s, int kind, const uint8_t *const *items, int n,
                         int a0)
{
    struct lrv_trace_event ev;
    if (!lrv_sys_trace_begin(s, &ev, kind))
        return;
    ev.items = items;
    ev.n = n;
    ev.a0 = a0;
    lrv_sys_trace_send(s, &ev);
}

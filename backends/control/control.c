/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Control mode (control.h): the lines written and the commands read.
 *
 * Output: one JSON object per line on stdout.
 *   {"ev":"<kind>","t":tick,"pos":offset|null,"s":"script","name":..,"text":"<hex>",
 *    "items":["<hex>",..],"ints":[..],"n":N,"a":[a0,a1,a2,a3]}
 *                     a trace event. <kind> is "wait", "error", "quit", "reset" or one of the
 *                     engine's names; texts are the bytes of the language's encoding
 *   {"ev":"stop","t":tick,"why":"start|blocked|break|ticks","wait":kind|-1,"pending":events}
 *   {"ev":"frame","t":tick,"hash":"<16 hex>","pal":"<hex>","gon":0|1,"scroll":N,
 *    "data":"<base64>"}                    (record; data only with record 2)
 *   {"ev":"snap","name":..,"t":tick,"hash":"<8 hex>","size":[W,H],"pal":..,"gon":..,
 *    "scroll":..,"data":..}                the engine's frame (shot.h)
 *   {"ev":"present","name":..,"t":tick,"size":[W,H],"win":[W,H],"dest":[x,y,w,h],"mode":M,
 *    "linear":0|1,"nonuniform":N,"border_lit":N,"data":"<base64 RGB>"}   the rendered frame
 *   {"ev":"ok"}                            after every command that does not resume the engine
 *   {"ev":"error","msg":..}                a command that was not understood
 *   {"ev":"exit","code":N,"msg":..}
 *
 * Commands, one per line:
 *   press K[,K..]     presses at the next poll. K is a key name of lrvne/input.h ("return",
 *                     "esc", "kp8", "f1", "a", ...), "ok" (return) or "cancel" (esc); the
 *                     character of a press is the US layout's with the held modifiers. Or an
 *                     event: quit emergency reset mouse_l mouse_r (a click: the edge, and the
 *                     button down for that one poll)
 *   keychr KEY HEX    one press of KEY with the character code HEX instead of the layout's
 *   hold KEY|mouse_l|mouse_r 0|1   the key-down bitmap and the button levels; shift, ctrl,
 *                     alt, caps and kana are also the modifiers of later presses
 *   mouse X Y         the host pointer is at (X, Y), in pixels of the video mode
 *   mouse_rel DX DY   the pointer moved by (DX, DY) and has no absolute position
 *   run [N]           resume; stop when blocked on an input wait, at a breakpoint, or after N
 *                     ticks
 *   break KIND|all    stop (after the tick) when an event of this kind was reported
 *   unbreak KIND|all
 *   mute KIND|all     do not write events of this kind (unmute KIND|all writes them again)
 *   blocked once|every  stop when blocked at every poll of the wait (default), or only at the
 *                     first poll after the engine entered it (a driver that answers every
 *                     wait at once saves a round trip per tick)
 *   record 0|1|2      a frame line per tick whose frame changed (2 = with the picture)
 *   snap NAME         write the displayed frame as the engine presented it
 *   readback NAME     write what the backend rendered for it (backends with a real display)
 *   quit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lrvne/core.h"
#include "autoplay.h"
#include "control.h"
#include "jsonout.h"
#include "shot.h"

#define NKINDS 256

static const char *const g_own_kinds[LRV_TR_ENGINE] = {
    [LRV_TR_WAIT] = "wait", [LRV_TR_ERROR] = "error", [LRV_TR_QUIT] = "quit",
    [LRV_TR_RESET] = "reset"
};

static const struct lrv_app *g_app;
static struct lrv_video_mode g_mode;
static struct lrv_shot g_shot;              /* the frame of the last present */

static uint32_t g_pending;                  /* LRV_EV_* edges for the next poll */
static struct lrv_keypress g_keys[LRV_KEYQ];/* key presses for the next poll */
static int g_nkeys;
static struct lrv_input g_levels;           /* keymap, pointer and buttons the driver set */
static unsigned g_click;                    /* buttons down for the next poll only */
static int g_mdx, g_mdy;                    /* relative motion for the next poll */

static long g_budget = -1;                  /* ticks to run before stopping; -1 = until blocked */
static uint8_t g_breaks[NKINDS];
static uint8_t g_muted[NKINDS];
static int g_block_once;                    /* `blocked once` */
static int g_wait_entered;                  /* a wait was entered since the last blocked stop */
static int g_break_hit;
static int g_record;
static uint64_t g_last_hash;
static int g_in_input_wait;                 /* the engine waits for the player's input */
static int g_wait_kind = -1;
static uint32_t g_tick;
static int g_started;                       /* the first run command was seen */
static int (*g_readback)(struct lrv_ctl_readback *rb);

/* ---- kinds -------------------------------------------------------------------------------- */

static const char *kind_name(int kind)
{
    if (kind > 0 && kind < LRV_TR_ENGINE)
        return g_own_kinds[kind];
    if (g_app && g_app->trace_names && kind >= LRV_TR_ENGINE &&
        kind - LRV_TR_ENGINE < g_app->ntrace_names)
        return g_app->trace_names[kind - LRV_TR_ENGINE];
    return 0;
}

static int kind_by_name(const char *s)
{
    int k, n = LRV_TR_ENGINE + (g_app ? g_app->ntrace_names : 0);
    for (k = 1; k < n && k < NKINDS; k++) {
        const char *name = kind_name(k);
        if (name && !strcmp(name, s))
            return k;
    }
    return -1;
}

static int wait_is_input(int wait_kind)
{
    return g_app && g_app->wait_is_input ? g_app->wait_is_input(wait_kind) != 0 : 1;
}

/* ---- output ------------------------------------------------------------------------------- */

static void out_end(void)
{
    putchar('\n');
    fflush(stdout);
}

static void emit_ok(void)
{
    printf("{\"ev\":\"ok\"}");
    out_end();
}

static void emit_error(const char *msg)
{
    printf("{\"ev\":\"error\",\"msg\":");
    lrv_json_str(stdout, msg);
    putchar('}');
    out_end();
}

static void emit_stop(const char *why)
{
    printf("{\"ev\":\"stop\",\"t\":%lu,\"why\":\"%s\",\"wait\":%d,\"pending\":%lu}",
           (unsigned long)g_tick, why, g_wait_kind, (unsigned long)g_pending);
    out_end();
}

static void emit_readback(const char *name)
{
    struct lrv_ctl_readback rb;
    size_t n = (size_t)g_mode.w * (size_t)g_mode.h * 3;

    if (!g_readback) {
        emit_error("no readback on this backend");
        return;
    }
    memset(&rb, 0, sizeof rb);
    rb.rgb = n ? malloc(n) : 0;
    if (!rb.rgb || g_readback(&rb) != 0) {
        free(rb.rgb);
        emit_error("readback failed");
        return;
    }
    printf("{\"ev\":\"present\",\"name\":");
    lrv_json_str(stdout, name);
    printf(",\"t\":%lu,\"size\":[%d,%d],\"win\":[%d,%d],\"dest\":[%d,%d,%d,%d],\"mode\":%d,"
           "\"linear\":%d,\"nonuniform\":%ld,\"border_lit\":%ld,\"data\":",
           (unsigned long)g_tick, g_mode.w, g_mode.h, rb.win_w, rb.win_h, rb.dst_x, rb.dst_y,
           rb.dst_w, rb.dst_h, rb.scale_mode, rb.linear, rb.nonuniform, rb.border_lit);
    lrv_json_b64(stdout, rb.rgb, n);
    putchar('}');
    out_end();
    free(rb.rgb);
    emit_ok();
}

/* ---- events ------------------------------------------------------------------------------- */

void lrv_ctl_trace(const struct lrv_trace_event *ev)
{
    const char *name = kind_name(ev->kind);
    int known = ev->kind > 0 && ev->kind < NKINDS, i;

    if (ev->kind == LRV_TR_WAIT && wait_is_input(ev->a0)) {
        g_in_input_wait = ev->a1 == 0;
        g_wait_entered |= ev->a1 == 0;
    }
    if (known && g_breaks[ev->kind])
        g_break_hit = ev->kind;
    if (known && g_muted[ev->kind])
        return;
    if (ev->kind == LRV_TR_WAIT)
        g_wait_kind = ev->a1 == 0 ? ev->a0 : -1;

    printf("{\"ev\":");
    lrv_json_str(stdout, name ? name : "?");
    printf(",\"t\":%lu,\"pos\":", (unsigned long)ev->tick);
    if (ev->pos == ~0u)
        printf("null");
    else
        printf("%lu", (unsigned long)ev->pos);
    printf(",\"s\":");
    lrv_json_str(stdout, ev->script ? ev->script : "");
    if (ev->name) {
        printf(",\"name\":");
        lrv_json_str(stdout, ev->name);
    }
    if (ev->text) {
        printf(",\"text\":");
        lrv_json_hex(stdout, ev->text, strlen((const char *)ev->text));
    }
    if (ev->items) {
        printf(",\"items\":[");
        for (i = 0; i < ev->n; i++) {
            if (i)
                putchar(',');
            lrv_json_hex(stdout, ev->items[i], strlen((const char *)ev->items[i]));
        }
        putchar(']');
    }
    if (ev->ints) {
        printf(",\"ints\":[");
        for (i = 0; i < ev->nints; i++)
            printf("%s%d", i ? "," : "", ev->ints[i]);
        putchar(']');
    }
    printf(",\"n\":%d,\"a\":[%d,%d,%d,%d]}", ev->n, ev->a0, ev->a1, ev->a2, ev->a3);
    out_end();
}

void lrv_ctl_video(const struct lrv_video_mode *mode)
{
    g_mode = *mode;
}

void lrv_ctl_present(const struct lrv_frame *frame)
{
    lrv_shot_capture(&g_shot, &g_mode, frame);
}

/* ---- commands ----------------------------------------------------------------------------- */

static void queue_key(int key, int chr)
{
    struct lrv_keypress *k;
    if (g_nkeys >= LRV_KEYQ)
        return;
    k = &g_keys[g_nkeys++];
    k->key = (uint8_t)key;
    k->chr = (uint8_t)(chr >= 0 ? chr : lrv_key_ascii(key, g_levels.mods));
    k->mods = (uint8_t)g_levels.mods;
}

static int cmd_press(char *list)
{
    char *tok;
    for (tok = list ? strtok(list, ",") : 0; tok; tok = strtok(0, ",")) {
        uint32_t event;
        int key;
        if (!lrv_input_by_name(tok, strlen(tok), &key, &event)) {
            emit_error("unknown key");
            return 0;
        }
        if (key != LRV_K_NONE)
            queue_key(key, -1);
        g_pending |= event;
        if (event & LRV_EV_MOUSE_L)
            g_click |= LRV_BTN_L;
        if (event & LRV_EV_MOUSE_R)
            g_click |= LRV_BTN_R;
    }
    emit_ok();
    return 0;
}

static void cmd_hold(const char *what, int on)
{
    uint32_t event;
    int key;

    if (!strcmp(what, "mouse_l") || !strcmp(what, "mouse_r")) {
        unsigned bit = what[6] == 'l' ? LRV_BTN_L : LRV_BTN_R;
        g_levels.buttons = on ? (g_levels.buttons | bit) : (g_levels.buttons & ~bit);
    } else if (lrv_input_by_name(what, strlen(what), &key, &event) && key != LRV_K_NONE) {
        unsigned mod = lrv_input_key_mod(key);
        lrv_key_set(&g_levels, key, on);
        g_levels.mods = on ? (g_levels.mods | mod) : (g_levels.mods & ~mod);
    } else {
        emit_error("unknown level");
        return;
    }
    emit_ok();
}

static void cmd_kinds(uint8_t *flags, const char *kind, int value)
{
    if (!kind) {
        emit_error("kind missing");
        return;
    }
    if (!strcmp(kind, "all")) {
        memset(flags, value, NKINDS);
    } else {
        int k = kind_by_name(kind);
        if (k < 0) {
            emit_error("unknown event kind");
            return;
        }
        flags[k] = (uint8_t)value;
    }
    emit_ok();
}

/* Returns 1 when the engine should resume. */
static int do_command(char *line)
{
    char *cmd = strtok(line, " \t\r\n");
    char *a1 = strtok(0, " \t\r\n");
    char *a2 = strtok(0, " \t\r\n");

    if (!cmd)
        return 0;
    if (!strcmp(cmd, "press")) {
        return cmd_press(a1);
    } else if (!strcmp(cmd, "keychr") && a1 && a2) {
        uint32_t event;
        int key;
        if (!lrv_input_by_name(a1, strlen(a1), &key, &event) || key == LRV_K_NONE) {
            emit_error("unknown key");
            return 0;
        }
        queue_key(key, (int)(strtol(a2, 0, 16) & 0xFF));
        emit_ok();
    } else if (!strcmp(cmd, "key")) {
        emit_error("key: scan codes are not known here; use press KEY or keychr KEY HEX");
    } else if (!strcmp(cmd, "hold") && a1 && a2) {
        cmd_hold(a1, atoi(a2) != 0);
    } else if (!strcmp(cmd, "mouse") && a1 && a2) {
        g_levels.mouse_abs = 1;
        g_levels.mouse_x = atoi(a1);
        g_levels.mouse_y = atoi(a2);
        emit_ok();
    } else if (!strcmp(cmd, "mouse_rel") && a1 && a2) {
        g_levels.mouse_abs = 0;
        g_mdx += atoi(a1);
        g_mdy += atoi(a2);
        emit_ok();
    } else if (!strcmp(cmd, "run")) {
        g_budget = a1 ? atol(a1) : -1;
        g_break_hit = 0;
        g_started = 1;
        return 1;
    } else if (!strcmp(cmd, "break") || !strcmp(cmd, "unbreak")) {
        cmd_kinds(g_breaks, a1, cmd[0] == 'b');
    } else if (!strcmp(cmd, "mute") || !strcmp(cmd, "unmute")) {
        cmd_kinds(g_muted, a1, cmd[0] == 'm');
    } else if (!strcmp(cmd, "blocked") && a1) {
        g_block_once = !strcmp(a1, "once");
        emit_ok();
    } else if (!strcmp(cmd, "record")) {
        g_record = a1 ? atoi(a1) : 1;
        g_last_hash = 0;
        emit_ok();
    } else if (!strcmp(cmd, "snap")) {
        if (g_shot.valid) {
            lrv_shot_emit_snap(stdout, &g_shot.frame, &g_shot.mode, a1 ? a1 : "snap", g_tick);
            emit_ok();
        } else {
            emit_error("no frame was presented yet");
        }
    } else if (!strcmp(cmd, "readback")) {
        emit_readback(a1 ? a1 : "readback");
    } else if (!strcmp(cmd, "quit")) {
        lrv_ctl_exit(0, "quit command");
    } else {
        emit_error("unknown command");
    }
    return 0;
}

static void read_commands(void)
{
    static char line[4096];
    for (;;) {
        if (!fgets(line, sizeof line, stdin))
            lrv_ctl_exit(4, "driver closed the pipe");
        if (do_command(line))
            return;
    }
}

/* ---- the frame ---------------------------------------------------------------------------- */

void lrv_ctl_tick(uint32_t tick)
{
    const char *why = 0;

    g_tick = tick;
    if (g_record && g_shot.valid) {
        uint64_t h = lrv_shot_hash64(&g_shot.frame, &g_shot.mode);
        if (h != g_last_hash) {
            g_last_hash = h;
            printf("{\"ev\":\"frame\",\"t\":%lu,\"hash\":\"%016llx\",", (unsigned long)tick,
                   (unsigned long long)h);
            lrv_shot_json_body(stdout, &g_shot.frame, &g_shot.mode, g_record >= 2);
            putchar('}');
            out_end();
        }
    }

    if (!g_started) {
        /* the very first tick: wait for the driver before the engine runs at all */
        emit_stop("start");
        read_commands();
        return;
    }
    if (g_budget > 0)
        g_budget--;
    if (g_break_hit)
        why = "break";
    else if (g_budget == 0)
        why = "ticks";
    else if (g_budget < 0 && g_in_input_wait && !g_pending && !g_nkeys && !g_mdx && !g_mdy &&
             (!g_block_once || g_wait_entered)) {
        why = "blocked";
        g_wait_entered = 0;
    }
    if (!why)
        return;
    emit_stop(why);
    read_commands();
}

void lrv_ctl_input(struct lrv_input *in)
{
    int i;
    *in = g_levels;
    in->events = g_pending;
    in->buttons |= g_click;
    in->mouse_dx = g_mdx;
    in->mouse_dy = g_mdy;
    in->nkeys = g_nkeys;
    for (i = 0; i < g_nkeys; i++)
        in->keys[i] = g_keys[i];
    g_pending = 0;
    g_click = 0;
    g_nkeys = 0;
    g_mdx = g_mdy = 0;
}

void lrv_ctl_input_flush(void)
{
    g_pending &= LRV_EV_STICKY;         /* the type-ahead is dropped; a quit never is */
    g_nkeys = 0;
    g_click = 0;
    g_mdx = g_mdy = 0;
}

void lrv_ctl_init(const struct lrv_app *app)
{
    g_app = app;
    memset(g_breaks, 0, sizeof g_breaks);
    memset(g_muted, 0, sizeof g_muted);
    memset(&g_levels, 0, sizeof g_levels);
}

void lrv_ctl_set_readback(int (*fn)(struct lrv_ctl_readback *rb))
{
    g_readback = fn;
}

void lrv_ctl_exit(int code, const char *msg)
{
    printf("{\"ev\":\"exit\",\"code\":%d,\"msg\":", code);
    lrv_json_str(stdout, msg ? msg : "");
    putchar('}');
    out_end();
    lrv_shot_free(&g_shot);
    exit(code);
}

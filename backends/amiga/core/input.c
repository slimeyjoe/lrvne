/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Window, IDCMP input as the neutral keys of lrvne/input.h, pointer and the screen front/back
 * policy.
 *
 * A borderless backdrop window covers the program's screen and receives raw keys and mouse
 * events. The Intuition screen shortcuts work untouched: LAmiga+N brings the Workbench to the
 * front and LAmiga+M cycles screens. When the screen goes to the back the pointer is shown
 * again and input stops being ours; when it comes back to the front the window is
 * re-activated and the pointer hidden. The engine keeps running either way (music
 * continues, as it would on the machine the engine models).
 *
 * Keys are taken by position (raw key codes), never by what the keymap prints on them; what a
 * key means is decided above this layer. A press carries the character a US layout gives
 * (lrv_key_ascii) and the modifiers in force. Typematic repeat is a key press. Shift, Ctrl
 * and Alt are not presses: they show in the key-down bitmap and in `mods`. The Amiga keys
 * are not delivered at all (Intuition's own). The keyboard's Help key is LRV_K_HELP.
 *
 * Locks: Caps Lock is the keyboard's own (LRV_MOD_CAPS, and LRV_K_CAPS down while it is on).
 * No Amiga keyboard has a kana key: right Amiga+K toggles a kana lock kept here
 * (LRV_MOD_KANA and LRV_K_KANA likewise).
 *
 * The backend's own keys never reach the engine:
 *   Shift+Esc          leave at once (LRV_EV_EMERGENCY)
 *   Ctrl+Shift+R       reset (LRV_EV_RESET)
 *   right Amiga+K      the kana lock
 *   F10 in a window    its next size
 * Ctrl-C from the shell and a window's close gadget ask to quit (LRV_EV_QUIT).
 *
 * The pointer. On the program's own screen it is hidden and only its motion is reported,
 * for a game that draws a pointer of its own; with app->host_pointer it stays visible and
 * its position is reported in pixels of the video mode. In a window the pointer is always
 * visible (the active window's pointer is the pointer of the whole screen) and both are
 * reported.
 *
 * The OS side (GetMsg/ReplyMsg, the front-screen check, Ctrl-C) runs in the main task's
 * once-per-frame OS call (frameos.h): the frame tick gathers the messages, the next poll
 * decodes them here. A poll with no gather since the last one makes its own call.
 *
 * In a window on a public screen (WINDOW, video_win.c: am_input_attach) the page's window is
 * the input window and the screen order does not matter. The click that activates the window
 * is delivered to it too: mouse buttons are ignored for two polls after an activation, or
 * that click would reach the game. */
#include "os_begin.h"
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include "os_end.h"
#include <string.h>
#include "amiga.h"
#include "frameos.h"

/* raw key codes used by name below */
#define RK_ESC      0x45
#define RK_F10      0x59
#define RK_LSHIFT   0x60
#define RK_RSHIFT   0x61
#define RK_CTRL     0x63
#define RK_LALT     0x64
#define RK_RALT     0x65

/* The key at each position of the keyboard, by raw key code (the codes of the Amiga keyboard,
 * with the additions later systems made for PC keyboards: Insert, Page Up/Down, F11, F12,
 * Pause, Home, End). 0 = a key the layer has no name for: the two keys only international
 * keyboards have (0x2B, 0x30), the keypad's brackets, the Amiga keys. */
static const uint8_t g_rawkey[128] = {
    /* 00 */ LRV_K_GRAVE, LRV_K_1, LRV_K_2, LRV_K_3, LRV_K_4, LRV_K_5, LRV_K_6, LRV_K_7,
    /* 08 */ LRV_K_8, LRV_K_9, LRV_K_0, LRV_K_MINUS, LRV_K_EQUALS, LRV_K_BACKSLASH, 0, LRV_K_KP0,
    /* 10 */ LRV_K_Q, LRV_K_W, LRV_K_E, LRV_K_R, LRV_K_T, LRV_K_Y, LRV_K_U, LRV_K_I,
    /* 18 */ LRV_K_O, LRV_K_P, LRV_K_LBRACKET, LRV_K_RBRACKET, 0, LRV_K_KP1, LRV_K_KP2, LRV_K_KP3,
    /* 20 */ LRV_K_A, LRV_K_S, LRV_K_D, LRV_K_F, LRV_K_G, LRV_K_H, LRV_K_J, LRV_K_K,
    /* 28 */ LRV_K_L, LRV_K_SEMICOLON, LRV_K_APOSTROPHE, 0, 0, LRV_K_KP4, LRV_K_KP5, LRV_K_KP6,
    /* 30 */ 0, LRV_K_Z, LRV_K_X, LRV_K_C, LRV_K_V, LRV_K_B, LRV_K_N, LRV_K_M,
    /* 38 */ LRV_K_COMMA, LRV_K_PERIOD, LRV_K_SLASH, 0, LRV_K_KP_PERIOD, LRV_K_KP7, LRV_K_KP8,
             LRV_K_KP9,
    /* 40 */ LRV_K_SPACE, LRV_K_BS, LRV_K_TAB, LRV_K_KP_ENTER, LRV_K_RETURN, LRV_K_ESC, LRV_K_DEL,
             LRV_K_INS,
    /* 48 */ LRV_K_PGUP, LRV_K_PGDN, LRV_K_KP_MINUS, LRV_K_F11, LRV_K_UP, LRV_K_DOWN, LRV_K_RIGHT,
             LRV_K_LEFT,
    /* 50 */ LRV_K_F1, LRV_K_F2, LRV_K_F3, LRV_K_F4, LRV_K_F5, LRV_K_F6, LRV_K_F7, LRV_K_F8,
    /* 58 */ LRV_K_F9, LRV_K_F10, 0, 0, LRV_K_KP_DIV, LRV_K_KP_MUL, LRV_K_KP_PLUS, LRV_K_HELP,
    /* 60 */ LRV_K_SHIFT, LRV_K_SHIFT, LRV_K_CAPS, LRV_K_CTRL, LRV_K_ALT, LRV_K_ALT, 0, 0,
    /* 68 */ 0, 0, 0, 0, 0, 0, LRV_K_PAUSE, LRV_K_F12,
    /* 70 */ LRV_K_HOME, LRV_K_END, 0, 0, 0, 0, 0, 0,
    /* 78 */ 0, 0, 0, 0, 0, 0, 0, 0
};

static struct Window *g_win;
static struct Screen *g_scr;
static UWORD *g_blank;              /* 1-line empty sprite = invisible pointer */
static struct lrv_input g_in;       /* events and presses accumulated since the last poll */
static uint8_t g_held[16];          /* raw key codes held (128 bits) */
static int g_caps, g_kana;          /* the locks */
static unsigned g_buttons;          /* LRV_BTN_* held */
static long g_rel_x, g_rel_y;       /* motion not yet handed out, in display pixels */
static int g_front = -1;            /* our screen is frontmost (tracked per tick) */
static int g_active;                /* our window is the active one */
static int g_pointer_hidden;
static int g_attached;              /* the window is video_win.c's (WINDOW) */
static int g_click_guard;           /* polls left in which mouse buttons are ignored */

static int raw_held(int k)
{
    return (g_held[k >> 3] >> (k & 7)) & 1;
}

static void hide_pointer(void)
{
    if (g_win && g_blank && !g_pointer_hidden && !g_attached && !am_app->host_pointer) {
        SetPointer(g_win, g_blank, 1, 16, 0, 0);
        g_pointer_hidden = 1;
    }
}

static void show_pointer(void)
{
    if (g_win && g_pointer_hidden) {
        ClearPointer(g_win);
        g_pointer_hidden = 0;
    }
}

static void reset_state(void)
{
    memset(&g_in, 0, sizeof g_in);
    memset(g_held, 0, sizeof g_held);
    g_caps = 0;
    g_buttons = 0;
    g_rel_x = g_rel_y = 0;
    g_active = 1;
    g_front = 1;
    g_pointer_hidden = 0;
    g_click_guard = 0;
}

static void frame_attach(void)
{
    am_fr->ibase = IntuitionBase;
    am_fr->scr = g_scr;
    am_fr->n_ev = am_fr->gathered = am_fr->more = am_fr->brk = 0;
    am_fr->win = g_win;
}

int am_input_open(struct Screen *scr)
{
    g_scr = scr;
    g_attached = 0;
    g_blank = AllocMem(12, MEMF_CHIP | MEMF_CLEAR);
    g_win = OpenWindowTags(NULL,
                           WA_CustomScreen, (IPTR)scr,
                           WA_Left, 0, WA_Top, 0, WA_Width, scr->Width, WA_Height, scr->Height,
                           WA_Flags, WFLG_BACKDROP | WFLG_BORDERLESS | WFLG_ACTIVATE |
                                     WFLG_RMBTRAP | WFLG_REPORTMOUSE | WFLG_SIMPLE_REFRESH |
                                     WFLG_NOCAREREFRESH,
                           WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE |
                                     IDCMP_DELTAMOVE | IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW,
                           TAG_DONE);
    if (!g_win) {
        am_log("input: cannot open window\n");
        if (g_blank)
            FreeMem(g_blank, 12);
        g_blank = 0;
        return -1;
    }
    reset_state();
    hide_pointer();
    frame_attach();
    return 0;
}

int am_input_attach(struct Window *win)
{
    g_win = win;
    g_scr = win->WScreen;
    g_blank = 0;
    g_attached = 1;
    reset_state();
    frame_attach();
    return 0;
}

void am_input_close(void)
{
    if (am_fr) {
        am_fr->win = 0;
        am_fr->n_ev = am_fr->gathered = am_fr->more = 0;
    }
    if (g_win) {
        struct IntuiMessage *m;
        show_pointer();
        while ((m = (struct IntuiMessage *)GetMsg(g_win->UserPort)) != NULL)
            ReplyMsg(&m->ExecMessage);
        if (!g_attached)
            CloseWindow(g_win);         /* an attached window is video_win.c's to close */
    }
    g_attached = 0;
    g_win = 0;
    if (g_blank)
        FreeMem(g_blank, 12);
    g_blank = 0;
}

/* the modifiers and locks in force: the keys held, the keyboard's caps lock, our kana lock */
static unsigned mods_now(void)
{
    unsigned mods = 0;
    if (raw_held(RK_LSHIFT) || raw_held(RK_RSHIFT))
        mods |= LRV_MOD_SHIFT;
    if (raw_held(RK_CTRL))
        mods |= LRV_MOD_CTRL;
    if (raw_held(RK_LALT) || raw_held(RK_RALT))
        mods |= LRV_MOD_ALT;
    if (g_caps)
        mods |= LRV_MOD_CAPS;
    if (g_kana)
        mods |= LRV_MOD_KANA;
    return mods;
}

static void key_event(UWORD code, UWORD qual)
{
    int down = !(code & 0x80), repeat = (qual & IEQUALIFIER_REPEAT) != 0;
    int k = code & 0x7f, key = g_rawkey[k];
    unsigned mods = 0;

    if (down)
        g_held[k >> 3] |= (uint8_t)(1 << (k & 7));
    else
        g_held[k >> 3] &= (uint8_t)~(1 << (k & 7));
    g_caps = (qual & IEQUALIFIER_CAPSLOCK) != 0;
    if (!down)
        return;
    /* the modifiers as the event carries them: a press is judged by what was held then */
    if (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT))
        mods |= LRV_MOD_SHIFT;
    if (qual & IEQUALIFIER_CONTROL)
        mods |= LRV_MOD_CTRL;
    if (qual & (IEQUALIFIER_LALT | IEQUALIFIER_RALT))
        mods |= LRV_MOD_ALT;
    if (g_caps)
        mods |= LRV_MOD_CAPS;

    /* the backend's own keys first */
    if (k == RK_ESC && (mods & LRV_MOD_SHIFT)) {
        g_in.events |= LRV_EV_EMERGENCY;
        return;
    }
    if (key == LRV_K_R && (mods & LRV_MOD_CTRL) && (mods & LRV_MOD_SHIFT)) {
        if (!repeat)
            g_in.events |= LRV_EV_RESET;
        return;
    }
    if (key == LRV_K_K && (qual & IEQUALIFIER_RCOMMAND)) {
        if (!repeat)
            g_kana = !g_kana;
        return;
    }
    if (k == RK_F10 && g_attached) {
        if (!repeat)
            am_win_cycle();
        return;
    }
    if (g_kana)
        mods |= LRV_MOD_KANA;
    if (key == LRV_K_NONE || key >= LRV_K_SHIFT || g_in.nkeys >= LRV_KEYQ)
        return;                         /* the modifiers only show in the bitmap */
    g_in.keys[g_in.nkeys].key = (uint8_t)key;
    g_in.keys[g_in.nkeys].chr = lrv_key_ascii(key, mods);
    g_in.keys[g_in.nkeys].mods = (uint8_t)mods;
    g_in.nkeys++;
}

/* the messages of the last gather */
static void decode(struct am_frameos *f)
{
    ULONG i;
    for (i = 0; i < f->n_ev; i++) {
        const struct am_frame_ev *e = &f->ev[i];
        UWORD code = (UWORD)e->code;
        switch (e->cls) {
        case IDCMP_RAWKEY:
            key_event(code, (UWORD)e->qual);
            break;
        case IDCMP_MOUSEMOVE:
            g_rel_x += (WORD)e->mx;
            g_rel_y += (WORD)e->my;
            break;
        case IDCMP_MOUSEBUTTONS:
            if (code == SELECTUP)
                g_buttons &= ~LRV_BTN_L;
            if (code == MENUUP)
                g_buttons &= ~LRV_BTN_R;
            if (g_click_guard)
                break;                            /* the click that activated the window */
            if (code == SELECTDOWN) {
                g_in.events |= LRV_EV_MOUSE_L;
                g_buttons |= LRV_BTN_L;
            }
            if (code == MENUDOWN) {
                g_in.events |= LRV_EV_MOUSE_R;
                g_buttons |= LRV_BTN_R;
            }
            break;
        case IDCMP_ACTIVEWINDOW:
            g_active = 1;
            if (g_attached)
                g_click_guard = 2;
            else
                hide_pointer();
            break;
        case IDCMP_CLOSEWINDOW:
            g_in.events |= LRV_EV_QUIT;
            break;
        case IDCMP_REFRESHWINDOW:
            am_win_refresh();
            break;
        case IDCMP_CHANGEWINDOW:
            am_win_changed();
            break;
        case IDCMP_INACTIVEWINDOW:
            g_active = 0;
            memset(g_held, 0, sizeof g_held);     /* we will not see the key-ups */
            g_buttons = 0;
            show_pointer();
            break;
        default:
            break;
        }
    }
    f->n_ev = 0;
    f->gathered = 0;
}

/* fresh: also gather what arrived since the frame tick's gather (a flush) */
static void pump(int fresh)
{
    struct am_frameos *f = am_fr;
    if (!g_win)
        return;
    if (fresh && f->gathered)
        decode(f);
    do {
        if (!f->gathered)
            am_frame_call(AM_F_INPUT);
        decode(f);
    } while (f->more);
    if (g_attached) {
        /* a window: the screen order is not ours to follow; the click guard runs down */
        if (g_click_guard)
            g_click_guard--;
    } else {
        /* screen front/back transitions (LAmiga+N / LAmiga+M, or another program's screen) */
        int front = f->front != 0;
        if (front != g_front) {
            g_front = front;
            if (front) {
                if (!g_active)
                    ActivateWindow(g_win);
                hide_pointer();
            } else {
                show_pointer();
            }
        }
    }
    if (f->brk) {
        f->brk = 0;
        g_in.events |= LRV_EV_QUIT;
    }
}

void am_input_poll(struct lrv_input *in)
{
    int k, sx;

    pump(0);
    memset(g_in.down, 0, sizeof g_in.down);
    for (k = 0; k < 128; k++)
        if (raw_held(k) && g_rawkey[k] != LRV_K_NONE && g_rawkey[k] != LRV_K_CAPS)
            lrv_key_set(&g_in, g_rawkey[k], 1);
    lrv_key_set(&g_in, LRV_K_CAPS, g_caps);
    lrv_key_set(&g_in, LRV_K_KANA, g_kana);
    g_in.mods = mods_now();
    g_in.buttons = g_buttons;
    /* motion in pixels of the video mode: whole pixels now, the rest later */
    sx = am_video_scale_x();
    g_in.mouse_dx = (int)(g_rel_x / sx);
    g_in.mouse_dy = (int)(g_rel_y / sx);
    g_rel_x -= (long)g_in.mouse_dx * sx;
    g_rel_y -= (long)g_in.mouse_dy * sx;
    /* the position where the host's pointer is the one the player sees */
    g_in.mouse_abs = 0;
    if (g_win && g_active && (g_attached || am_app->host_pointer) &&
        am_video_to_page(g_win->MouseX, g_win->MouseY, &g_in.mouse_x, &g_in.mouse_y))
        g_in.mouse_abs = 1;

    *in = g_in;
    g_in.events = 0;
    g_in.nkeys = 0;
}

void am_input_flush(void)
{
    pump(1);
    g_in.events &= LRV_EV_STICKY;       /* a quit is never lost to a flush */
    g_in.nkeys = 0;
    g_rel_x = g_rel_y = 0;
}

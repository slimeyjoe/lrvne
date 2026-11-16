/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SDL3 backend, input: host events as the neutral keys of lrvne/input.h.
 *
 * Keys are taken by position (SDL scancodes), never by what the host's layout prints on them:
 * the machines the engines model read their keyboards that way, and what a key means is
 * decided above this layer. A press carries the character a US layout gives for the key
 * (lrv_key_ascii) and the modifiers in force. Auto-repeat is a key press. The modifier keys
 * themselves are not presses: they show in the key-down bitmap and in `mods`.
 *
 * Locks: Caps Lock is reported as the host keeps it (LRV_MOD_CAPS, and LRV_K_CAPS down while
 * it is on, as a key that latches mechanically reads). The Kana key of a JIS keyboard, or
 * Scroll Lock on keyboards without one, toggles a kana lock kept here (LRV_MOD_KANA and
 * LRV_K_KANA likewise).
 *
 * The backend's own keys never reach the engine:
 *   F11, Alt+Return    fullscreen
 *   F12                the next scale mode
 *   Ctrl+Shift+R       reset (LRV_EV_RESET)
 *   Shift+Esc          leave at once (LRV_EV_EMERGENCY)
 *
 * The pointer is absolute, in pixels of the video mode, wherever the host pointer is over the
 * window; the motion since the previous poll is given too, in the same pixels. */
#include <string.h>
#include "sdl3.h"

static struct lrv_input g_in;           /* accumulated since the last poll */
static int g_kana;                      /* the kana lock */
static float g_rel_x, g_rel_y;          /* motion not yet handed out, fractions kept */

/* the key at a position of the keyboard; LRV_K_NONE for keys the layer has no name for */
static int key_of(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return LRV_K_A + (int)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0)
        return LRV_K_1 + (int)(sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10)
        return LRV_K_F1 + (int)(sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F15)
        return LRV_K_F13 + (int)(sc - SDL_SCANCODE_F13);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return LRV_K_KP1 + (int)(sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_RETURN:         return LRV_K_RETURN;
    case SDL_SCANCODE_ESCAPE:         return LRV_K_ESC;
    case SDL_SCANCODE_BACKSPACE:      return LRV_K_BS;
    case SDL_SCANCODE_TAB:            return LRV_K_TAB;
    case SDL_SCANCODE_SPACE:          return LRV_K_SPACE;
    case SDL_SCANCODE_MINUS:          return LRV_K_MINUS;
    case SDL_SCANCODE_EQUALS:         return LRV_K_EQUALS;
    case SDL_SCANCODE_LEFTBRACKET:    return LRV_K_LBRACKET;
    case SDL_SCANCODE_RIGHTBRACKET:   return LRV_K_RBRACKET;
    case SDL_SCANCODE_BACKSLASH:      return LRV_K_BACKSLASH;
    case SDL_SCANCODE_SEMICOLON:      return LRV_K_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE:     return LRV_K_APOSTROPHE;
    case SDL_SCANCODE_GRAVE:          return LRV_K_GRAVE;
    case SDL_SCANCODE_COMMA:          return LRV_K_COMMA;
    case SDL_SCANCODE_PERIOD:         return LRV_K_PERIOD;
    case SDL_SCANCODE_SLASH:          return LRV_K_SLASH;
    case SDL_SCANCODE_INTERNATIONAL3: return LRV_K_YEN;
    case SDL_SCANCODE_INTERNATIONAL1: return LRV_K_RO;
    case SDL_SCANCODE_INSERT:         return LRV_K_INS;
    case SDL_SCANCODE_DELETE:         return LRV_K_DEL;
    case SDL_SCANCODE_HOME:           return LRV_K_HOME;
    case SDL_SCANCODE_END:            return LRV_K_END;
    case SDL_SCANCODE_PAGEUP:         return LRV_K_PGUP;
    case SDL_SCANCODE_PAGEDOWN:       return LRV_K_PGDN;
    case SDL_SCANCODE_UP:             return LRV_K_UP;
    case SDL_SCANCODE_DOWN:           return LRV_K_DOWN;
    case SDL_SCANCODE_LEFT:           return LRV_K_LEFT;
    case SDL_SCANCODE_RIGHT:          return LRV_K_RIGHT;
    case SDL_SCANCODE_PAUSE:          return LRV_K_PAUSE;
    case SDL_SCANCODE_HELP:           return LRV_K_HELP;
    case SDL_SCANCODE_KP_0:           return LRV_K_KP0;
    case SDL_SCANCODE_KP_ENTER:       return LRV_K_KP_ENTER;
    case SDL_SCANCODE_KP_PLUS:        return LRV_K_KP_PLUS;
    case SDL_SCANCODE_KP_MINUS:       return LRV_K_KP_MINUS;
    case SDL_SCANCODE_KP_MULTIPLY:    return LRV_K_KP_MUL;
    case SDL_SCANCODE_KP_DIVIDE:      return LRV_K_KP_DIV;
    case SDL_SCANCODE_KP_PERIOD:      return LRV_K_KP_PERIOD;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:         return LRV_K_SHIFT;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:          return LRV_K_CTRL;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:           return LRV_K_ALT;
    default:                          return LRV_K_NONE;
    }
}

static unsigned mods_of(SDL_Keymod mod)
{
    unsigned mods = g_kana ? LRV_MOD_KANA : 0;
    if (mod & SDL_KMOD_SHIFT)
        mods |= LRV_MOD_SHIFT;
    if (mod & SDL_KMOD_CTRL)
        mods |= LRV_MOD_CTRL;
    if (mod & SDL_KMOD_ALT)
        mods |= LRV_MOD_ALT;
    if (mod & SDL_KMOD_CAPS)
        mods |= LRV_MOD_CAPS;
    return mods;
}

static void handle_key(const SDL_KeyboardEvent *k)
{
    SDL_Keymod mod = k->mod;
    unsigned mods;
    int key;

    /* the backend's own keys first */
    if ((k->scancode == SDL_SCANCODE_RETURN && (mod & SDL_KMOD_ALT)) ||
        k->scancode == SDL_SCANCODE_F11) {
        if (!k->repeat)
            lrv_sdl3_toggle_fullscreen();
        return;
    }
    if (k->scancode == SDL_SCANCODE_F12) {
        if (!k->repeat)
            lrv_sdl3_cycle_scale();
        return;
    }
    if (k->scancode == SDL_SCANCODE_ESCAPE && (mod & SDL_KMOD_SHIFT)) {
        g_in.events |= LRV_EV_EMERGENCY;
        return;
    }
    if (k->scancode == SDL_SCANCODE_R && (mod & SDL_KMOD_CTRL) && (mod & SDL_KMOD_SHIFT)) {
        if (!k->repeat)
            g_in.events |= LRV_EV_RESET;
        return;
    }
    if (k->scancode == SDL_SCANCODE_INTERNATIONAL2 || k->scancode == SDL_SCANCODE_SCROLLLOCK) {
        if (!k->repeat)
            g_kana = !g_kana;
        return;
    }

    key = key_of(k->scancode);
    if (key == LRV_K_NONE || key >= LRV_K_SHIFT || g_in.nkeys >= LRV_KEYQ)
        return;                         /* the modifiers only show in the bitmap */
    mods = mods_of(mod);
    g_in.keys[g_in.nkeys].key = (uint8_t)key;
    g_in.keys[g_in.nkeys].chr = lrv_key_ascii(key, mods);
    g_in.keys[g_in.nkeys].mods = (uint8_t)mods;
    g_in.nkeys++;
}

static void pointer_at(float wx, float wy)
{
    if (lrv_sdl3_window_to_mode(wx, wy, &g_in.mouse_x, &g_in.mouse_y))
        g_in.mouse_abs = 1;
}

static void pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            g_in.events |= LRV_EV_QUIT;
            break;
        case SDL_EVENT_KEY_DOWN:
            handle_key(&e.key);
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            float dx, dy;
            pointer_at(e.motion.x, e.motion.y);
            lrv_sdl3_window_to_mode_rel(e.motion.xrel, e.motion.yrel, &dx, &dy);
            g_rel_x += dx;
            g_rel_y += dy;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            pointer_at(e.button.x, e.button.y);
            if (e.button.button == SDL_BUTTON_LEFT)
                g_in.events |= LRV_EV_MOUSE_L;
            if (e.button.button == SDL_BUTTON_RIGHT)
                g_in.events |= LRV_EV_MOUSE_R;
            break;
        default:
            lrv_sdl3_video_event(&e);
            break;
        }
    }
}

void lrv_sdl3_input_poll(struct lrv_input *in)
{
    const bool *keys;
    SDL_MouseButtonFlags mb;
    int nkeys, i;

    pump_events();
    if (lrv_sdl3.control) {             /* the driver's input, not the host's */
        lrv_ctl_input(in);
        if (lrv_sdl3.out_of_ticks)
            in->events |= LRV_EV_EMERGENCY;
        return;
    }
    memset(g_in.down, 0, sizeof g_in.down);
    keys = SDL_GetKeyboardState(&nkeys);
    for (i = 0; keys && i < nkeys; i++)
        if (keys[i]) {
            int key = key_of((SDL_Scancode)i);
            if (key != LRV_K_NONE)
                lrv_key_set(&g_in, key, 1);
        }
    g_in.mods = mods_of(SDL_GetModState());
    lrv_key_set(&g_in, LRV_K_CAPS, (g_in.mods & LRV_MOD_CAPS) != 0);
    lrv_key_set(&g_in, LRV_K_KANA, g_kana);
    mb = SDL_GetMouseState(NULL, NULL);
    g_in.buttons = ((mb & SDL_BUTTON_LMASK) ? LRV_BTN_L : 0) |
                   ((mb & SDL_BUTTON_RMASK) ? LRV_BTN_R : 0);
    g_in.mouse_dx = (int)g_rel_x;       /* whole pixels now, the rest later */
    g_in.mouse_dy = (int)g_rel_y;
    g_rel_x -= (float)g_in.mouse_dx;
    g_rel_y -= (float)g_in.mouse_dy;

    *in = g_in;
    if (lrv_sdl3.have_autoplay)
        lrv_autoplay_poll(&lrv_sdl3.autoplay, lrv_sdl3.frames, in);
    if (lrv_sdl3.out_of_ticks)
        in->events |= LRV_EV_EMERGENCY;
    g_in.events = 0;
    g_in.nkeys = 0;
}

void lrv_sdl3_input_flush(void)
{
    pump_events();
    g_in.events &= LRV_EV_STICKY;       /* a quit is never lost to a flush */
    g_in.nkeys = 0;
    if (lrv_sdl3.control)
        lrv_ctl_input_flush();
    if (lrv_sdl3.have_autoplay)
        lrv_autoplay_flush(&lrv_sdl3.autoplay, lrv_sdl3.frames);
}

void lrv_sdl3_mouse_warp(int x, int y)
{
    SDL_Window *win = lrv_sdl3_window();
    float wx, wy;

    if (lrv_sdl3.control || !win || !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS))
        return;
    if (lrv_sdl3_mode_to_window(x, y, &wx, &wy))
        SDL_WarpMouseInWindow(win, wx, wy);
}

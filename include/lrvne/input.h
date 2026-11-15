/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Input as a backend delivers it: host-neutral keys, pointer and a few requests.
 *
 * Keys are named by their position on a keyboard, not by what a layout prints on them, the way
 * the machines the engines model read their keyboards. What a key means is decided above this
 * layer: a machine module turns the keys into its own scan codes (lrvne/pc98.h), or an engine
 * maps them to its commands directly. A backend never knows either.
 *
 * Locks. Caps and kana are states, not keys that are held: down[LRV_K_CAPS] and
 * down[LRV_K_KANA] are 1 while the lock is on, as a key that latches mechanically reads, and
 * always equal LRV_MOD_CAPS and LRV_MOD_KANA in `mods`. Caps is the host's own lock. Kana is a
 * lock the backend keeps and toggles with a key of its choice (the host's kana key, Scroll
 * Lock, ...), since few hosts have one. That toggle is the backend's own key and is never
 * delivered as a press; LRV_K_CAPS and LRV_K_KANA appear in `keys` only where the host has
 * that physical key and it was pressed, and nothing above may depend on such a press: the
 * state is what counts. Shift, Ctrl and Alt likewise are levels and modifiers, not presses. */
#ifndef LRVNE_INPUT_H
#define LRVNE_INPUT_H

#include <stdint.h>

enum lrv_key {
    LRV_K_NONE = 0,
    /* letters and the digit row, contiguous */
    LRV_K_A, LRV_K_B, LRV_K_C, LRV_K_D, LRV_K_E, LRV_K_F, LRV_K_G, LRV_K_H, LRV_K_I,
    LRV_K_J, LRV_K_K, LRV_K_L, LRV_K_M, LRV_K_N, LRV_K_O, LRV_K_P, LRV_K_Q, LRV_K_R,
    LRV_K_S, LRV_K_T, LRV_K_U, LRV_K_V, LRV_K_W, LRV_K_X, LRV_K_Y, LRV_K_Z,
    LRV_K_1, LRV_K_2, LRV_K_3, LRV_K_4, LRV_K_5, LRV_K_6, LRV_K_7, LRV_K_8, LRV_K_9, LRV_K_0,
    /* the other keys of the typing area, by position on a US keyboard */
    LRV_K_RETURN, LRV_K_ESC, LRV_K_BS, LRV_K_TAB, LRV_K_SPACE,
    LRV_K_MINUS, LRV_K_EQUALS, LRV_K_LBRACKET, LRV_K_RBRACKET, LRV_K_BACKSLASH,
    LRV_K_SEMICOLON, LRV_K_APOSTROPHE, LRV_K_GRAVE, LRV_K_COMMA, LRV_K_PERIOD, LRV_K_SLASH,
    LRV_K_YEN,              /* JIS keyboards: left of backspace */
    LRV_K_RO,               /* JIS keyboards: left of the right shift */
    /* function keys, contiguous */
    LRV_K_F1, LRV_K_F2, LRV_K_F3, LRV_K_F4, LRV_K_F5, LRV_K_F6, LRV_K_F7, LRV_K_F8,
    LRV_K_F9, LRV_K_F10, LRV_K_F11, LRV_K_F12, LRV_K_F13, LRV_K_F14, LRV_K_F15,
    /* editing and cursor keys */
    LRV_K_INS, LRV_K_DEL, LRV_K_HOME, LRV_K_END, LRV_K_PGUP, LRV_K_PGDN,
    LRV_K_UP, LRV_K_DOWN, LRV_K_LEFT, LRV_K_RIGHT,
    LRV_K_PAUSE, LRV_K_HELP,
    /* keypad, digits contiguous */
    LRV_K_KP0, LRV_K_KP1, LRV_K_KP2, LRV_K_KP3, LRV_K_KP4, LRV_K_KP5, LRV_K_KP6, LRV_K_KP7,
    LRV_K_KP8, LRV_K_KP9, LRV_K_KP_ENTER, LRV_K_KP_PLUS, LRV_K_KP_MINUS, LRV_K_KP_MUL,
    LRV_K_KP_DIV, LRV_K_KP_PERIOD,
    /* modifiers (either side) and locks */
    LRV_K_SHIFT, LRV_K_CTRL, LRV_K_ALT, LRV_K_CAPS, LRV_K_KANA,
    LRV_K_COUNT
};

#define LRV_KEYMAP_BYTES 16     /* key-down bitmap: LRV_K_COUNT <= 128 */
#define LRV_KEYQ         16     /* key presses one poll delivers at most */

/* modifiers and locks in force when a key was pressed, and as levels in lrv_input.mods */
#define LRV_MOD_SHIFT (1u << 0)
#define LRV_MOD_CTRL  (1u << 1)
#define LRV_MOD_ALT   (1u << 2)
#define LRV_MOD_CAPS  (1u << 3)     /* caps lock is on; down[LRV_K_CAPS] says the same */
#define LRV_MOD_KANA  (1u << 4)     /* kana lock is on; down[LRV_K_KANA] says the same */

struct lrv_keypress {
    uint8_t key;            /* enum lrv_key */
    uint8_t chr;            /* the ASCII character a US layout gives with the modifiers, 0 = none */
    uint8_t mods;           /* LRV_MOD_* at the time of the press */
};

/* Edge events (set for one poll when they happened). */
#define LRV_EV_QUIT      (1u << 0)  /* the host asked to close (window close, break) */
#define LRV_EV_EMERGENCY (1u << 1)  /* leave at once, without asking */
#define LRV_EV_MOUSE_L   (1u << 2)  /* button press edges: a click shorter than a poll counts */
#define LRV_EV_MOUSE_R   (1u << 3)
#define LRV_EV_RESET     (1u << 4)  /* the machine's reset button: the game starts again */
/* the events input_flush must keep for the next poll */
#define LRV_EV_STICKY    (LRV_EV_QUIT | LRV_EV_EMERGENCY | LRV_EV_RESET)

#define LRV_BTN_L 1u
#define LRV_BTN_R 2u

struct lrv_input {
    uint32_t events;                /* LRV_EV_* since the previous poll */
    int nkeys;                      /* key presses since the previous poll, oldest first;
                                     * auto-repeat is a key press */
    struct lrv_keypress keys[LRV_KEYQ];
    uint8_t down[LRV_KEYMAP_BYTES]; /* key-down bitmap: bit (key & 7) of byte key >> 3; for
                                     * LRV_K_CAPS and LRV_K_KANA the lock's state */
    unsigned mods;                  /* level: LRV_MOD_* now */
    int mouse_abs;                  /* 1: mouse_x/mouse_y is where the host pointer is */
    int mouse_x, mouse_y;           /* absolute pointer, in pixels of the video mode */
    int mouse_dx, mouse_dy;         /* relative motion since the previous poll, same pixels */
    unsigned buttons;               /* level: LRV_BTN_* held */
};

static inline int lrv_key_down(const struct lrv_input *in, int key)
{
    return (in->down[(key >> 3) & (LRV_KEYMAP_BYTES - 1)] >> (key & 7)) & 1;
}

static inline void lrv_key_set(struct lrv_input *in, int key, int down)
{
    if (down)
        in->down[(key >> 3) & (LRV_KEYMAP_BYTES - 1)] |= (uint8_t)(1u << (key & 7));
    else
        in->down[(key >> 3) & (LRV_KEYMAP_BYTES - 1)] &= (uint8_t)~(1u << (key & 7));
}

/* 1 when any key is held. */
static inline int lrv_any_key_down(const struct lrv_input *in)
{
    int i;
    for (i = 0; i < LRV_KEYMAP_BYTES; i++)
        if (in->down[i])
            return 1;
    return 0;
}

/* The name of a key ("a", "return", "kp8", "f1", ...) for option parsing, test scripts and the
 * control protocol, and the key of a name (any case); NULL / LRV_K_NONE when there is none. */
const char *lrv_key_name(int key);
int lrv_key_by_name(const char *name);

/* The ASCII character a US layout gives for a key with LRV_MOD_SHIFT / LRV_MOD_CAPS, 0 for
 * keys that type nothing. Backends use it to fill lrv_keypress.chr the same way everywhere. */
uint8_t lrv_key_ascii(int key, unsigned mods);

#endif

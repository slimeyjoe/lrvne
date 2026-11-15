/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The NEC PC-9801 as the engines see it: its graphics mode and its keyboard.
 *
 * Engines for PC-98 originals read the keyboard the way the machine presents it: scan codes
 * with the character its BIOS pairs with them, and a key-down bitmap indexed by scan code.
 * This module turns the neutral keys a backend delivers (lrvne/input.h) into that. A backend
 * knows nothing of it.
 *
 * Host keys are taken by position: letters, digits, Return, Space, Esc, Tab, BS, Delete, the
 * keypad digits, F1..F10, Shift, Ctrl and Caps Lock are themselves; Pause = STOP, Alt = GRPH,
 * Home = HOME CLR, End (or a Help key) = HELP, Page Up / Down = ROLL DOWN / ROLL UP; the two
 * keys right of P are the PC-98's @ and [, the key right of ; is its : and the US backslash
 * key its ]; the JIS keyboard's yen and ro keys are the PC-98's yen and _. CAPS and KANA are
 * locks, as lrvne/input.h has them: in the bitmap while on, never a key press. */
#ifndef LRVNE_PC98_H
#define LRVNE_PC98_H

#include <stdint.h>
#include "lrvne/hal.h"
#include "lrvne/input.h"

/* ---- video ------------------------------------------------------------------------------ */

#define LRV_PC98_W        640
#define LRV_PC98_H        400
#define LRV_PC98_STRIDE   80                            /* bytes per line of a plane */
#define LRV_PC98_FRAME_NS 17724193u                     /* 56.42 Hz vertical refresh */

/* The 640x400 16-colour graphics mode: four planes, shown 4:3, 16 colours of 4 bits a gun. */
extern const struct lrv_video_mode lrv_pc98_video_mode;

/* ---- keyboard --------------------------------------------------------------------------- */

/* Scan codes (00h..7Fh) of the keys the engines name. */
enum lrv_pc98_scan {
    LRV_PC98_K_ESC = 0x00, LRV_PC98_K_1 = 0x01, LRV_PC98_K_0 = 0x0A, LRV_PC98_K_MINUS = 0x0B,
    LRV_PC98_K_CARET = 0x0C, LRV_PC98_K_YEN = 0x0D, LRV_PC98_K_BS = 0x0E, LRV_PC98_K_TAB = 0x0F,
    LRV_PC98_K_Q = 0x10, LRV_PC98_K_Y = 0x15, LRV_PC98_K_AT = 0x1A, LRV_PC98_K_LBRACKET = 0x1B,
    LRV_PC98_K_RETURN = 0x1C, LRV_PC98_K_A = 0x1D, LRV_PC98_K_SEMICOLON = 0x26,
    LRV_PC98_K_COLON = 0x27, LRV_PC98_K_RBRACKET = 0x28, LRV_PC98_K_Z = 0x29,
    LRV_PC98_K_N = 0x2E, LRV_PC98_K_COMMA = 0x30, LRV_PC98_K_PERIOD = 0x31,
    LRV_PC98_K_SLASH = 0x32, LRV_PC98_K_UNDERSCORE = 0x33, LRV_PC98_K_SPACE = 0x34,
    LRV_PC98_K_XFER = 0x35, LRV_PC98_K_ROLLUP = 0x36, LRV_PC98_K_ROLLDOWN = 0x37,
    LRV_PC98_K_INS = 0x38, LRV_PC98_K_DEL = 0x39, LRV_PC98_K_UP = 0x3A, LRV_PC98_K_LEFT = 0x3B,
    LRV_PC98_K_RIGHT = 0x3C, LRV_PC98_K_DOWN = 0x3D, LRV_PC98_K_HOME = 0x3E,
    LRV_PC98_K_HELP = 0x3F,
    LRV_PC98_K_KP7 = 0x42, LRV_PC98_K_KP8 = 0x43, LRV_PC98_K_KP9 = 0x44, LRV_PC98_K_KP4 = 0x46,
    LRV_PC98_K_KP5 = 0x47, LRV_PC98_K_KP6 = 0x48, LRV_PC98_K_KP1 = 0x4A, LRV_PC98_K_KP2 = 0x4B,
    LRV_PC98_K_KP3 = 0x4C, LRV_PC98_K_KP0 = 0x4E,
    LRV_PC98_K_STOP = 0x60, LRV_PC98_K_COPY = 0x61, LRV_PC98_K_F1 = 0x62, LRV_PC98_K_F10 = 0x6B,
    /* the shift keys: they are in the bitmap but never a key press */
    LRV_PC98_K_SHIFT = 0x70, LRV_PC98_K_CAPS = 0x71, LRV_PC98_K_KANA = 0x72,
    LRV_PC98_K_GRPH = 0x73, LRV_PC98_K_CTRL = 0x74
};

#define LRV_PC98_K_NONE      0xFF   /* not a PC-98 key */
#define LRV_PC98_KEYMAP_BYTES 16    /* key-down bitmap: bit (scan & 7) of byte scan >> 3 */

/* Options of the translation. */
#define LRV_PC98_CURSOR_KEYPAD (1u << 0)    /* the host's cursor keys are the keypad's 8/4/6/2
                                             * (for originals that read only the keypad), not
                                             * the PC-98's own cursor keys */

struct lrv_pc98_key {
    uint8_t scan;           /* scan code 00h..6Bh */
    uint8_t chr;            /* character code: ASCII, JIS X 0201 kana in kana mode, 0 = none */
};

/* The scan code of a key (enum lrv_key), LRV_PC98_K_NONE when the PC-98 has no such key. */
uint8_t lrv_pc98_scan(int key, unsigned flags);

/* The character the BIOS pairs with a key press, by the key and LRV_MOD_SHIFT / LRV_MOD_CAPS:
 * letters in the case the two give, the digits and - , . / (the digits only unshifted), the
 * keypad digits, and the codes of Return (0Dh), Space, Esc (1Bh), Tab (09h), BS (08h) and
 * Delete (7Fh); 0 for everything else. lrv_keypress.chr is not used: it follows a US layout. */
uint8_t lrv_pc98_char(int key, unsigned mods);

/* A key press as the PC-98 delivers it. Returns 1 and fills *out, or 0 when the key is not a
 * PC-98 key or is a shift key (those only change the bitmap). With LRV_MOD_KANA in the press's
 * modifiers the keys of the typing area give half-width kana by the JIS kana layout (shifted:
 * the small kana and the brackets). */
int lrv_pc98_keypress(const struct lrv_keypress *k, unsigned flags, struct lrv_pc98_key *out);

/* The key-down bitmap of a poll: every held key at its scan code, and CAPS and KANA as the
 * locks they are (down while the lock is on). */
void lrv_pc98_keymap(const struct lrv_input *in, unsigned flags,
                     uint8_t map[LRV_PC98_KEYMAP_BYTES]);

static inline int lrv_pc98_key_down(const uint8_t map[LRV_PC98_KEYMAP_BYTES], int scan)
{
    return (map[(scan >> 3) & 15] >> (scan & 7)) & 1;
}

#endif

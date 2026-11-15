/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The PC-98's graphics mode and keyboard translation (lrvne/pc98.h). */
#include <string.h>
#include "lrvne/pc98.h"
#include "lrvne/surface.h"

const struct lrv_video_mode lrv_pc98_video_mode = {
    LRV_FMT_PLANAR4, LRV_PC98_W, LRV_PC98_H, 4, 3, LRV_PC98_FRAME_NS, 16, 4
};

/* scan code + 1 of every neutral key, 0 = the PC-98 has no such key. Letters, the digits 1-9,
 * F1-F10 and the keypad digits are computed. */
#define S(scan) ((uint8_t)((scan) + 1))
static const uint8_t scan_of[LRV_K_COUNT] = {
    [LRV_K_0] = S(LRV_PC98_K_0),
    [LRV_K_RETURN] = S(LRV_PC98_K_RETURN), [LRV_K_ESC] = S(LRV_PC98_K_ESC),
    [LRV_K_BS] = S(LRV_PC98_K_BS), [LRV_K_TAB] = S(LRV_PC98_K_TAB),
    [LRV_K_SPACE] = S(LRV_PC98_K_SPACE),
    [LRV_K_MINUS] = S(LRV_PC98_K_MINUS), [LRV_K_EQUALS] = S(LRV_PC98_K_CARET),
    [LRV_K_LBRACKET] = S(LRV_PC98_K_AT), [LRV_K_RBRACKET] = S(LRV_PC98_K_LBRACKET),
    [LRV_K_BACKSLASH] = S(LRV_PC98_K_RBRACKET), [LRV_K_SEMICOLON] = S(LRV_PC98_K_SEMICOLON),
    [LRV_K_APOSTROPHE] = S(LRV_PC98_K_COLON), [LRV_K_COMMA] = S(LRV_PC98_K_COMMA),
    [LRV_K_PERIOD] = S(LRV_PC98_K_PERIOD), [LRV_K_SLASH] = S(LRV_PC98_K_SLASH),
    [LRV_K_YEN] = S(LRV_PC98_K_YEN), [LRV_K_RO] = S(LRV_PC98_K_UNDERSCORE),
    [LRV_K_INS] = S(LRV_PC98_K_INS), [LRV_K_DEL] = S(LRV_PC98_K_DEL),
    [LRV_K_HOME] = S(LRV_PC98_K_HOME), [LRV_K_END] = S(LRV_PC98_K_HELP),
    [LRV_K_PGUP] = S(LRV_PC98_K_ROLLDOWN), [LRV_K_PGDN] = S(LRV_PC98_K_ROLLUP),
    [LRV_K_UP] = S(LRV_PC98_K_UP), [LRV_K_DOWN] = S(LRV_PC98_K_DOWN),
    [LRV_K_LEFT] = S(LRV_PC98_K_LEFT), [LRV_K_RIGHT] = S(LRV_PC98_K_RIGHT),
    [LRV_K_PAUSE] = S(LRV_PC98_K_STOP), [LRV_K_HELP] = S(LRV_PC98_K_HELP),
    [LRV_K_KP_ENTER] = S(LRV_PC98_K_RETURN),
    [LRV_K_SHIFT] = S(LRV_PC98_K_SHIFT), [LRV_K_CTRL] = S(LRV_PC98_K_CTRL),
    [LRV_K_ALT] = S(LRV_PC98_K_GRPH), [LRV_K_CAPS] = S(LRV_PC98_K_CAPS),
    [LRV_K_KANA] = S(LRV_PC98_K_KANA)
};
#undef S

static const uint8_t letter_scan[26] = {            /* A..Z */
    0x1D, 0x2D, 0x2B, 0x1F, 0x12, 0x20, 0x21, 0x22, 0x17, 0x23, 0x24, 0x25, 0x2F,
    0x2E, 0x18, 0x19, 0x10, 0x13, 0x1E, 0x14, 0x16, 0x2C, 0x11, 0x2A, 0x15, 0x29
};

static const uint8_t keypad_scan[10] = {            /* 0..9 */
    LRV_PC98_K_KP0, LRV_PC98_K_KP1, LRV_PC98_K_KP2, LRV_PC98_K_KP3, LRV_PC98_K_KP4,
    LRV_PC98_K_KP5, LRV_PC98_K_KP6, LRV_PC98_K_KP7, LRV_PC98_K_KP8, LRV_PC98_K_KP9
};

/* the keyboard in kana mode: the JIS kana layout by scan code 00h..33h, unshifted and shifted
 * (the small kana and the brackets), as JIS X 0201 bytes; 0 = the key gives no kana */
#define KANA_KEYS 0x34
static const uint8_t kana_layout[2][KANA_KEYS] = {
    {
        0x00, 0xC7, 0xCC, 0xB1, 0xB3, 0xB4, 0xB5, 0xD4, 0xD5, 0xD6, 0xDC, 0xCE, 0xCD,
        0xB0, 0x00, 0x00, 0xC0, 0xC3, 0xB2, 0xBD, 0xB6, 0xDD, 0xC5, 0xC6, 0xD7, 0xBE,
        0xDE, 0xDF, 0x00, 0xC1, 0xC4, 0xBC, 0xCA, 0xB7, 0xB8, 0xCF, 0xC9, 0xD8, 0xDA,
        0xB9, 0xD1, 0xC2, 0xBB, 0xBF, 0xCB, 0xBA, 0xD0, 0xD3, 0xC8, 0xD9, 0xD2, 0xDB
    }, {
        0x00, 0xC7, 0xCC, 0xA7, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xA6, 0xCE, 0xCD,
        0xB0, 0x00, 0x00, 0xC0, 0xC3, 0xA8, 0xBD, 0xB6, 0xDD, 0xC5, 0xC6, 0xD7, 0xBE,
        0xDE, 0xA2, 0x00, 0xC1, 0xC4, 0xBC, 0xCA, 0xB7, 0xB8, 0xCF, 0xC9, 0xD8, 0xDA,
        0xB9, 0xA3, 0xAF, 0xBB, 0xBF, 0xCB, 0xBA, 0xD0, 0xD3, 0xA4, 0xA1, 0xA5, 0xDB
    }
};

uint8_t lrv_pc98_scan(int key, unsigned flags)
{
    if (key >= LRV_K_A && key <= LRV_K_Z)
        return letter_scan[key - LRV_K_A];
    if (key >= LRV_K_1 && key <= LRV_K_9)
        return (uint8_t)(LRV_PC98_K_1 + (key - LRV_K_1));
    if (key >= LRV_K_F1 && key <= LRV_K_F10)
        return (uint8_t)(LRV_PC98_K_F1 + (key - LRV_K_F1));
    if (key >= LRV_K_KP0 && key <= LRV_K_KP9)
        return keypad_scan[key - LRV_K_KP0];
    if (flags & LRV_PC98_CURSOR_KEYPAD) {
        switch (key) {
        case LRV_K_UP:    return LRV_PC98_K_KP8;
        case LRV_K_LEFT:  return LRV_PC98_K_KP4;
        case LRV_K_RIGHT: return LRV_PC98_K_KP6;
        case LRV_K_DOWN:  return LRV_PC98_K_KP2;
        default:          break;
        }
    }
    if (key <= LRV_K_NONE || key >= LRV_K_COUNT || !scan_of[key])
        return LRV_PC98_K_NONE;
    return (uint8_t)(scan_of[key] - 1);
}

uint8_t lrv_pc98_char(int key, unsigned mods)
{
    int shift = (mods & LRV_MOD_SHIFT) != 0, caps = (mods & LRV_MOD_CAPS) != 0;
    if (key >= LRV_K_A && key <= LRV_K_Z)
        return (uint8_t)((shift != caps ? 'A' : 'a') + (key - LRV_K_A));
    if (key >= LRV_K_1 && key <= LRV_K_9)
        return shift ? 0 : (uint8_t)('1' + (key - LRV_K_1));
    if (key >= LRV_K_KP0 && key <= LRV_K_KP9)
        return (uint8_t)('0' + (key - LRV_K_KP0));
    switch (key) {
    case LRV_K_0:        return shift ? 0 : '0';
    case LRV_K_RETURN:
    case LRV_K_KP_ENTER: return 0x0D;
    case LRV_K_SPACE:    return 0x20;
    case LRV_K_ESC:      return 0x1B;
    case LRV_K_TAB:      return 0x09;
    case LRV_K_BS:       return 0x08;
    case LRV_K_DEL:      return 0x7F;
    case LRV_K_MINUS:    return '-';
    case LRV_K_COMMA:    return ',';
    case LRV_K_PERIOD:   return '.';
    case LRV_K_SLASH:    return '/';
    default:             return 0;
    }
}

int lrv_pc98_keypress(const struct lrv_keypress *k, unsigned flags, struct lrv_pc98_key *out)
{
    uint8_t scan = lrv_pc98_scan(k->key, flags);
    uint8_t chr;
    if (scan == LRV_PC98_K_NONE || scan >= LRV_PC98_K_SHIFT)
        return 0;
    chr = lrv_pc98_char(k->key, k->mods);
    if ((k->mods & LRV_MOD_KANA) && scan < KANA_KEYS && kana_layout[0][scan])
        chr = kana_layout[(k->mods & LRV_MOD_SHIFT) ? 1 : 0][scan];
    out->scan = scan;
    out->chr = chr;
    return 1;
}

static void map_set(uint8_t *map, int scan)
{
    map[(scan >> 3) & 15] |= (uint8_t)(1u << (scan & 7));
}

void lrv_pc98_keymap(const struct lrv_input *in, unsigned flags,
                     uint8_t map[LRV_PC98_KEYMAP_BYTES])
{
    int key;
    memset(map, 0, LRV_PC98_KEYMAP_BYTES);
    for (key = LRV_K_NONE + 1; key < LRV_K_COUNT; key++)
        if (lrv_key_down(in, key)) {
            uint8_t scan = lrv_pc98_scan(key, flags);
            if (scan != LRV_PC98_K_NONE)
                map_set(map, scan);
        }
    if (in->mods & LRV_MOD_CAPS)
        map_set(map, LRV_PC98_K_CAPS);
    if (in->mods & LRV_MOD_KANA)
        map_set(map, LRV_PC98_K_KANA);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Key names and the US-layout characters of keys (lrvne/input.h). */
#include "lrvne/core.h"
#include "lrvne/input.h"

/* unshifted and shifted character of the keys from LRV_K_1 to LRV_K_SLASH */
static const char plain[] =   "1234567890\r\033\b\t -=[]\\;'`,./";
static const char shifted[] = "!@#$%^&*()\r\033\b\t _+{}|:\"~<>?";

static const char *const names[LRV_K_COUNT] = {
    [LRV_K_RETURN] = "return", [LRV_K_ESC] = "esc", [LRV_K_BS] = "bs", [LRV_K_TAB] = "tab",
    [LRV_K_SPACE] = "space", [LRV_K_MINUS] = "minus", [LRV_K_EQUALS] = "equals",
    [LRV_K_LBRACKET] = "lbracket", [LRV_K_RBRACKET] = "rbracket",
    [LRV_K_BACKSLASH] = "backslash", [LRV_K_SEMICOLON] = "semicolon",
    [LRV_K_APOSTROPHE] = "apostrophe", [LRV_K_GRAVE] = "grave", [LRV_K_COMMA] = "comma",
    [LRV_K_PERIOD] = "period", [LRV_K_SLASH] = "slash", [LRV_K_YEN] = "yen", [LRV_K_RO] = "ro",
    [LRV_K_INS] = "ins", [LRV_K_DEL] = "del", [LRV_K_HOME] = "home", [LRV_K_END] = "end",
    [LRV_K_PGUP] = "pgup", [LRV_K_PGDN] = "pgdn", [LRV_K_UP] = "up", [LRV_K_DOWN] = "down",
    [LRV_K_LEFT] = "left", [LRV_K_RIGHT] = "right", [LRV_K_PAUSE] = "pause",
    [LRV_K_HELP] = "help", [LRV_K_KP_ENTER] = "kpenter", [LRV_K_KP_PLUS] = "kpplus",
    [LRV_K_KP_MINUS] = "kpminus", [LRV_K_KP_MUL] = "kpmul", [LRV_K_KP_DIV] = "kpdiv",
    [LRV_K_KP_PERIOD] = "kpperiod", [LRV_K_SHIFT] = "shift", [LRV_K_CTRL] = "ctrl",
    [LRV_K_ALT] = "alt", [LRV_K_CAPS] = "caps", [LRV_K_KANA] = "kana"
};

/* the names of the contiguous groups: one buffer each, valid until the next call would be
 * wrong for a function returning constants, so they are spelled out */
static const char *const letters[26] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"
};
static const char *const digits[10] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };
static const char *const fkeys[15] = {
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15"
};
static const char *const keypad[10] = {
    "kp0", "kp1", "kp2", "kp3", "kp4", "kp5", "kp6", "kp7", "kp8", "kp9"
};

const char *lrv_key_name(int key)
{
    if (key >= LRV_K_A && key <= LRV_K_Z)
        return letters[key - LRV_K_A];
    if (key >= LRV_K_1 && key <= LRV_K_0)
        return digits[key - LRV_K_1];
    if (key >= LRV_K_F1 && key <= LRV_K_F15)
        return fkeys[key - LRV_K_F1];
    if (key >= LRV_K_KP0 && key <= LRV_K_KP9)
        return keypad[key - LRV_K_KP0];
    if (key > LRV_K_NONE && key < LRV_K_COUNT)
        return names[key];
    return 0;
}

int lrv_key_by_name(const char *name)
{
    int k;
    for (k = LRV_K_NONE + 1; k < LRV_K_COUNT; k++) {
        const char *n = lrv_key_name(k);
        if (n && lrv_name_eq(n, name))
            return k;
    }
    return LRV_K_NONE;
}

uint8_t lrv_key_ascii(int key, unsigned mods)
{
    int shift = (mods & LRV_MOD_SHIFT) != 0, caps = (mods & LRV_MOD_CAPS) != 0;
    if (key >= LRV_K_A && key <= LRV_K_Z)
        return (uint8_t)((shift != caps ? 'A' : 'a') + (key - LRV_K_A));
    if (key >= LRV_K_1 && key <= LRV_K_SLASH)
        return (uint8_t)(shift ? shifted : plain)[key - LRV_K_1];
    if (key >= LRV_K_KP0 && key <= LRV_K_KP9)
        return (uint8_t)('0' + (key - LRV_K_KP0));
    switch (key) {
    case LRV_K_DEL:       return 0x7F;
    case LRV_K_KP_ENTER:  return '\r';
    case LRV_K_KP_PLUS:   return '+';
    case LRV_K_KP_MINUS:  return '-';
    case LRV_K_KP_MUL:    return '*';
    case LRV_K_KP_DIV:    return '/';
    case LRV_K_KP_PERIOD: return '.';
    default:              return 0;
    }
}

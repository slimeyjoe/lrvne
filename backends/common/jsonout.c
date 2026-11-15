/* SPDX-License-Identifier: GPL-3.0-or-later */
/* JSON output helpers (jsonout.h). */
#include "jsonout.h"

void lrv_json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c < 0x20 || c >= 0x7F) {
            fprintf(f, "\\u%04x", (unsigned)c);
        } else {
            fputc(c, f);
        }
    }
    fputc('"', f);
}

void lrv_json_hex(FILE *f, const uint8_t *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    fputc('"', f);
    for (i = 0; i < n; i++) {
        fputc(digits[p[i] >> 4], f);
        fputc(digits[p[i] & 15], f);
    }
    fputc('"', f);
}

void lrv_json_b64(FILE *f, const uint8_t *p, size_t n)
{
    static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    fputc('"', f);
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        fputc(tb[v >> 18], f);
        fputc(tb[(v >> 12) & 63], f);
        fputc(tb[(v >> 6) & 63], f);
        fputc(tb[v & 63], f);
    }
    if (i + 1 == n) {
        uint32_t v = (uint32_t)p[i] << 16;
        fputc(tb[v >> 18], f);
        fputc(tb[(v >> 12) & 63], f);
        fputc('=', f);
        fputc('=', f);
    } else if (i + 2 == n) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
        fputc(tb[v >> 18], f);
        fputc(tb[(v >> 12) & 63], f);
        fputc(tb[(v >> 6) & 63], f);
        fputc('=', f);
    }
    fputc('"', f);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shift-JIS (cp932) helpers: the lead-byte test and the standard conversion of a two-byte
 * character to its JIS X 0208 code, which is what the kanji ROMs and the glyph tables are
 * keyed by. */
#ifndef LRVNE_SJIS_H
#define LRVNE_SJIS_H

#include <stdint.h>

/* 1 when b starts a two-byte character (81h..9Fh, E0h..FCh). */
static inline int lrv_sjis_is_lead(unsigned b)
{
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

/* The 16-bit JIS code (row and cell bytes 21h..7Eh each) of a Shift-JIS pair. */
static inline unsigned lrv_sjis_to_jis(unsigned hi, unsigned lo)
{
    hi -= (hi <= 0x9F) ? 0x71 : 0xB1;
    hi = hi * 2 + 1;
    if (lo > 0x7F)
        lo--;
    if (lo >= 0x9E) {
        lo -= 0x7D;
        hi++;
    } else {
        lo -= 0x1F;
    }
    return ((hi & 0xFF) << 8) | (lo & 0xFF);
}

#endif

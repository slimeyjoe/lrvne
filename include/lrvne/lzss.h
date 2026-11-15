/* SPDX-License-Identifier: GPL-3.0-or-later */
/* LZSS decoder: Okumura's 1989 scheme with its usual parameters, as countless game files of
 * the era were packed. A 4096-byte ring, matches of 3 to 18 bytes, a zero-filled ring whose
 * write position starts at 4078; the stream is groups of a flag byte (LSB first, 1 = a
 * literal byte follows, 0 = a two-byte match: position low byte, then position bits 8-11 in
 * the high nibble and length - 3 in the low nibble) and its eight items.
 *
 * Only the raw stream is decoded here. Whatever a file format puts around it (a header with
 * the unpacked size, a checksum) is the engine's that reads the format. */
#ifndef LRVNE_LZSS_H
#define LRVNE_LZSS_H

#include <stddef.h>
#include <stdint.h>

#define LRV_LZSS_RING 4096

/* Decodes the stream src[0 .. srclen) into dst and returns the number of bytes produced.
 * Decoding ends with the input, also in the middle of an item; output beyond dstcap is
 * dropped (the count returned never exceeds dstcap). `ring` is the caller's work area of
 * LRV_LZSS_RING bytes, so that the decoder needs no large stack frame. */
size_t lrv_lzss_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstcap,
                       uint8_t *ring);

#endif

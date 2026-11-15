/* SPDX-License-Identifier: GPL-3.0-or-later */
/* <string.h> for the freestanding build of tests/opn68k: what src/snd/opn_lite.c uses. */
#ifndef LRVNE_TESTS_OPN68K_STRING_H
#define LRVNE_TESTS_OPN68K_STRING_H

#include <stddef.h>

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#endif

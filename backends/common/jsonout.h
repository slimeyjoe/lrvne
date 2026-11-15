/* SPDX-License-Identifier: GPL-3.0-or-later */
/* JSON output helpers of the backends: the pieces of the lines the test automation protocol
 * and the snapshot printer write. */
#ifndef LRVNE_BACKENDS_JSONOUT_H
#define LRVNE_BACKENDS_JSONOUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* A JSON string: quotes and backslashes escaped, bytes outside printable ASCII as \u00XX. */
void lrv_json_str(FILE *f, const char *s);
/* Bytes as a string of lower-case hexadecimal digits. */
void lrv_json_hex(FILE *f, const uint8_t *p, size_t n);
/* Bytes as a base64 string. */
void lrv_json_b64(FILE *f, const uint8_t *p, size_t n);

#endif

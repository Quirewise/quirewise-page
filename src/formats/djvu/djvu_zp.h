/* First-party implementation of the DjVu ZP binary arithmetic decoder.
 *
 * The ZP coder is the adaptive binary arithmetic coder used by DjVu for BZZ,
 * JB2 and IW44 streams. This module implements the decode side only, as
 * described by the DjVu format specification; it is not derived from any
 * third-party library source. */

#ifndef QUIREWISE_PAGE_DJVU_ZP_H
#define QUIREWISE_PAGE_DJVU_ZP_H

#include <stddef.h>
#include <stdint.h>

/* Initial probability and state-transition table (format constant). */
typedef struct QwDjvuZpTable {
    uint16_t p;
    uint16_t m;
    uint8_t up;
    uint8_t dn;
} QwDjvuZpTable;

extern const QwDjvuZpTable qw_djvu_zp_default_table[256];

typedef struct QwDjvuZp {
    const uint8_t *data;
    size_t size;
    size_t pos;

    uint32_t a;
    uint32_t code;
    uint32_t fence;
    uint32_t buffer;
    uint8_t scount;
    uint8_t delay;
    uint8_t zbyte;
    int eof;

    uint32_t p[256];
    uint32_t m[256];
    uint8_t up[256];
    uint8_t dn[256];
} QwDjvuZp;

void qw_djvu_zp_init(QwDjvuZp *zp, const uint8_t *data, size_t size);

/* Decode one bit through an adaptive context (one byte of state). */
int qw_djvu_zp_decode(QwDjvuZp *zp, uint8_t *ctx);

/* Decode one bit with a fixed 0.5 probability context (no adaptation). */
int qw_djvu_zp_decode_pass(QwDjvuZp *zp);

/* Decode one bit with the IW44 fixed probability (0.5 + 3a/8). */
int qw_djvu_zp_decode_iw(QwDjvuZp *zp);

#endif

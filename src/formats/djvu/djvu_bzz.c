/* First-party DjVu BZZ decoder.
 *
 * A BZZ stream is a sequence of blocks. Each block carries a 24-bit size, a
 * quasi-MTF coded byte sequence (adaptive ZP coder plus frequency-ordered
 * alphabet) terminated by a marker byte, and is reconstructed with an inverse
 * Burrows-Wheeler transform. This module is written from the DjVu format
 * description; it shares only the ZP primitive with the other DjVu decoders. */

#include "djvu_bzz.h"
#include "djvu_zp.h"

#include <stdlib.h>
#include <string.h>

#define QW_DJVU_BZZ_MAX_BLOCK ((size_t)4096u * 1024u)
#define QW_DJVU_BZZ_FREQ_MAX 4
#define QW_DJVU_BZZ_CTX_IDS 3
#define QW_DJVU_BZZ_CTX_COUNT 384

typedef struct QwDjvuBzz {
    QwDjvuZp zp;
    uint8_t ctx[QW_DJVU_BZZ_CTX_COUNT];
    uint8_t *block;
    size_t block_cap;
    uint32_t *pos;
    size_t pos_cap;
} QwDjvuBzz;

static int bzz_decode_raw(QwDjvuBzz *bzz, int bits)
{
    int n = 1;
    int m = 1 << bits;
    while (n < m) n = (n << 1) | qw_djvu_zp_decode_pass(&bzz->zp);
    return n - m;
}

static int bzz_decode_binary(QwDjvuBzz *bzz, int ctxoff, int bits)
{
    int n = 1;
    int m = 1 << bits;
    --ctxoff;
    while (n < m) {
        n = (n << 1) | qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + n]);
    }
    return n - m;
}

static int bzz_grow_block(QwDjvuBzz *bzz, size_t size)
{
    uint8_t *block;
    if (bzz->block_cap >= size) return 0;
    block = (uint8_t *)realloc(bzz->block, size);
    if (!block) return QW_DJVU_BZZ_ERR_MEMORY;
    bzz->block = block;
    bzz->block_cap = size;
    return 0;
}

static int bzz_grow_pos(QwDjvuBzz *bzz, size_t count)
{
    uint32_t *pos;
    if (bzz->pos_cap >= count) return 0;
    pos = (uint32_t *)realloc(bzz->pos, count * sizeof(uint32_t));
    if (!pos) return QW_DJVU_BZZ_ERR_MEMORY;
    bzz->pos = pos;
    bzz->pos_cap = count;
    return 0;
}

/* Decode one block into bzz->block. Returns the block length, 0 at end of
 * stream, or a negative error code. */
static int bzz_decode_block(QwDjvuBzz *bzz)
{
    int size = bzz_decode_raw(bzz, 24);
    int fshift = 0;
    int fadd = 4;
    int mtfno = 3;
    int markerpos = -1;
    int i;
    int k;
    int last;
    uint8_t mtf[256];
    int freq[QW_DJVU_BZZ_FREQ_MAX];
    int count[256];
    uint8_t *data;
    int rc;

    if (size == 0) return 0;
    if (size < 0 || (size_t)size > QW_DJVU_BZZ_MAX_BLOCK)
        return QW_DJVU_BZZ_ERR_FORMAT;

    rc = bzz_grow_block(bzz, (size_t)size);
    if (rc != 0) return rc;
    data = bzz->block;

    /* Estimation speed. */
    if (qw_djvu_zp_decode_pass(&bzz->zp) != 0) {
        ++fshift;
        if (qw_djvu_zp_decode_pass(&bzz->zp) != 0) ++fshift;
    }

    for (i = 0; i < 256; ++i) mtf[i] = (uint8_t)i;
    for (i = 0; i < QW_DJVU_BZZ_FREQ_MAX; ++i) freq[i] = 0;

    for (i = 0; i < size; ++i) {
        int ctxid = QW_DJVU_BZZ_CTX_IDS - 1;
        int ctxoff = 0;
        int fc;

        if (ctxid > mtfno) ctxid = mtfno;

        if (qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + ctxid]) != 0) {
            mtfno = 0;
            data[i] = mtf[mtfno];
        } else if ((ctxoff += QW_DJVU_BZZ_CTX_IDS,
                    qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + ctxid]) != 0)) {
            mtfno = 1;
            data[i] = mtf[mtfno];
        } else if ((ctxoff += QW_DJVU_BZZ_CTX_IDS,
                    qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + 0]) != 0)) {
            mtfno = 2 + bzz_decode_binary(bzz, ctxoff + 1, 1);
            data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 1),
                    qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + 0]) != 0)) {
            mtfno = 4 + bzz_decode_binary(bzz, ctxoff + 1, 2);
            data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 3),
                    qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + 0]) != 0)) {
            mtfno = 8 + bzz_decode_binary(bzz, ctxoff + 1, 3);
            data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 7),
                    qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + 0]) != 0)) {
            mtfno = 16 + bzz_decode_binary(bzz, ctxoff + 1, 4);
            data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 15),
                    qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + 0]) != 0)) {
            mtfno = 32 + bzz_decode_binary(bzz, ctxoff + 1, 5);
            data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 31),
                    qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + 0]) != 0)) {
            mtfno = 64 + bzz_decode_binary(bzz, ctxoff + 1, 6);
            data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 63),
                    qw_djvu_zp_decode(&bzz->zp, &bzz->ctx[ctxoff + 0]) != 0)) {
            mtfno = 128 + bzz_decode_binary(bzz, ctxoff + 1, 7);
            data[i] = mtf[mtfno];
        } else {
            mtfno = 256;
            data[i] = 0;
            markerpos = i;
            continue;
        }

        /* Rotate the quasi-MTF alphabet by empirical frequency. */
        fadd = fadd + (fadd >> fshift);
        if (fadd > 0x10000000) {
            fadd >>= 24;
            freq[0] >>= 24;
            freq[1] >>= 24;
            freq[2] >>= 24;
            freq[3] >>= 24;
        }
        fc = fadd;
        if (mtfno < QW_DJVU_BZZ_FREQ_MAX) fc += freq[mtfno];
        for (k = mtfno; k >= QW_DJVU_BZZ_FREQ_MAX; --k) mtf[k] = mtf[k - 1];
        for (; k > 0 && (uint32_t)fc >= (uint32_t)freq[k - 1]; --k) {
            mtf[k] = mtf[k - 1];
            freq[k] = freq[k - 1];
        }
        mtf[k] = data[i];
        freq[k] = fc;
    }

    /* Inverse Burrows-Wheeler transform (marker excluded from the output). */
    if (markerpos < 1 || markerpos >= size) return QW_DJVU_BZZ_ERR_FORMAT;

    rc = bzz_grow_pos(bzz, (size_t)size);
    if (rc != 0) return rc;

    for (i = 0; i < 256; ++i) count[i] = 0;
    for (i = 0; i < markerpos; ++i) {
        uint32_t byte = data[i];
        bzz->pos[i] = (byte << 24) | (uint32_t)(count[byte] & 0xffffff);
        ++count[byte];
    }
    for (i = markerpos + 1; i < size; ++i) {
        uint32_t byte = data[i];
        bzz->pos[i] = (byte << 24) | (uint32_t)(count[byte] & 0xffffff);
        ++count[byte];
    }

    last = 1;
    for (i = 0; i < 256; ++i) {
        int tmp = count[i];
        count[i] = last;
        last += tmp;
    }

    {
        int index = 0;
        int out = size - 1;
        while (out > 0) {
            uint32_t packed;
            uint32_t byte;
            int next;
            if (index < 0 || index >= size) return QW_DJVU_BZZ_ERR_FORMAT;
            packed = bzz->pos[index];
            byte = (packed >> 24) & 0xffu;
            data[--out] = (uint8_t)byte;
            next = count[byte] + (int)(packed & 0xffffffu);
            index = next;
        }
        if (index != markerpos) return QW_DJVU_BZZ_ERR_FORMAT;
    }
    return size - 1;
}

int qw_djvu_bzz_decode(const uint8_t *data, size_t size,
                       uint8_t **out_data, size_t *out_size,
                       size_t max_output)
{
    QwDjvuBzz bzz;
    uint8_t *out = NULL;
    size_t out_len = 0;
    size_t out_cap = 0;
    int rc = QW_DJVU_BZZ_OK;

    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!data || !out_data || !out_size) return QW_DJVU_BZZ_ERR_FORMAT;

    memset(&bzz, 0, sizeof(bzz));
    qw_djvu_zp_init(&bzz.zp, data, size);

    for (;;) {
        int n = bzz_decode_block(&bzz);
        if (n < 0) { rc = n; break; }
        if (n == 0) break;
        if ((size_t)n > max_output || out_len > max_output - (size_t)n) {
            rc = QW_DJVU_BZZ_ERR_LIMIT;
            break;
        }
        if (out_len + (size_t)n > out_cap) {
            size_t cap = out_cap ? out_cap : 65536u;
            uint8_t *grown;
            while (cap < out_len + (size_t)n) cap *= 2u;
            grown = (uint8_t *)realloc(out, cap + 1u);
            if (!grown) { rc = QW_DJVU_BZZ_ERR_MEMORY; break; }
            out = grown;
            out_cap = cap;
        }
        memcpy(out + out_len, bzz.block, (size_t)n);
        out_len += (size_t)n;
    }

    free(bzz.block);
    free(bzz.pos);

    if (rc != QW_DJVU_BZZ_OK) {
        free(out);
        return rc;
    }
    if (!out) {
        out = (uint8_t *)malloc(1u);
        if (!out) return QW_DJVU_BZZ_ERR_MEMORY;
    }
    out[out_len] = 0;
    *out_data = out;
    *out_size = out_len;
    return QW_DJVU_BZZ_OK;
}

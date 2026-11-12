/* First-party DjVu IW44 wavelet decoder.
 *
 * The decoder keeps three coefficient maps (Y, Cb, Cr). Each chunk carries a
 * number of "slices"; every slice codes one wavelet band of every 32x32
 * macroblock. Coefficients are stored sparsely as 16-entry buckets in a
 * 64-bucket block, scattered into a padded coefficient plane, reconstructed
 * with the inverse 5/3 wavelet filter, and converted to RGB samples. */

#include "djvu_iw44.h"
#include "djvu_zp.h"

#include <stdlib.h>
#include <string.h>

#define QW_IW44_MAX_PIXELS ((size_t)100u * 1000u * 1000u)

static const int band_start[10] = { 0, 1, 2, 3, 4, 8, 12, 16, 32, 48 };
static const int band_size[10] = { 1, 1, 1, 1, 4, 4, 4, 16, 16, 16 };

static const int iwquant[16] = {
    0x10000, 0x20000, 0x20000, 0x40000, 0x40000, 0x40000, 0x80000,
    0x80000, 0x80000, 0x100000, 0x100000, 0x100000, 0x200000,
    0x100000, 0x100000, 0x200000
};

typedef struct IwBlock {
    int16_t *buckets[64];
} IwBlock;

typedef struct IwMap {
    int w, h;
    int bw, bh;
    int nb;
    IwBlock *blocks;
} IwMap;

typedef struct IwCodec {
    int8_t coeff_state[256];
    int8_t bucket_state[16];
    uint8_t ctx_start[32];
    uint8_t ctx_bucket[10][8];
    uint8_t ctx_mant;
    uint8_t ctx_root;
    int quant_high[10];
    int quant_low[16];
    int curband;
    int curbit;
} IwCodec;

struct QwDjvuIw44Image {
    int w, h;
    int serial;
    int cslices;
    int crcbdelay;
    int crcbhalf;
    int has_color;
    IwMap *ymap;
    IwMap *cbmap;
    IwMap *crmap;
    IwCodec yc;
    IwCodec cbc;
    IwCodec crc;
};

/* ------------------------------------------------------------------ */
/* Sparse coefficient maps                                             */
/* ------------------------------------------------------------------ */

static IwMap *map_new(int w, int h)
{
    IwMap *map;
    int bw, bh, nb;

    if (w <= 0 || h <= 0) return NULL;
    if ((size_t)w * (size_t)h > QW_IW44_MAX_PIXELS) return NULL;
    bw = (w + 31) & ~31;
    bh = (h + 31) & ~31;
    if (bw <= 0 || bh <= 0) return NULL;
    nb = (bw / 32) * (bh / 32);
    map = (IwMap *)calloc(1, sizeof(*map));
    if (!map) return NULL;
    map->w = w;
    map->h = h;
    map->bw = bw;
    map->bh = bh;
    map->nb = nb;
    map->blocks = (IwBlock *)calloc((size_t)nb, sizeof(IwBlock));
    if (!map->blocks) {
        free(map);
        return NULL;
    }
    return map;
}

static void map_free(IwMap *map)
{
    int b, k;
    if (!map) return;
    for (b = 0; b < map->nb; ++b) {
        for (k = 0; k < 64; ++k) free(map->blocks[b].buckets[k]);
    }
    free(map->blocks);
    free(map);
}

static int16_t *bucket_get(IwBlock *block, int index)
{
    if (!block->buckets[index]) {
        block->buckets[index] = (int16_t *)calloc(16, sizeof(int16_t));
    }
    return block->buckets[index];
}

/* ------------------------------------------------------------------ */
/* Codec setup and slice decoding                                      */
/* ------------------------------------------------------------------ */

static int next_quant(IwCodec *c)
{
    int flag = 0;
    int i;
    for (i = 0; i < 16; ++i) {
        c->quant_low[i] >>= 1;
        if (c->quant_low[i] != 0) flag = 1;
    }
    for (i = 0; i < 10; ++i) {
        c->quant_high[i] >>= 1;
        if (c->quant_high[i] != 0) flag = 1;
    }
    return flag;
}

static void codec_init(IwCodec *c)
{
    int i;
    int qidx = 0;

    memset(c, 0, sizeof(*c));
    c->curband = 0;
    c->curbit = 1;

    i = 0;
    while (i < 4) c->quant_low[i++] = iwquant[qidx++];
    while (i < 8) c->quant_low[i++] = iwquant[qidx];
    ++qidx;
    while (i < 12) c->quant_low[i++] = iwquant[qidx];
    ++qidx;
    while (i < 16) c->quant_low[i++] = iwquant[qidx];
    ++qidx;
    c->quant_high[0] = 0;
    for (i = 1; i < 10; ++i) c->quant_high[i] = iwquant[qidx++];
    while (c->quant_low[0] >= 32768) next_quant(c);
}

static int is_null_slice(IwCodec *c, int band)
{
    int i;

    if (band == 0) {
        int is_null = 1;
        for (i = 0; i < 16; ++i) {
            int threshold = c->quant_low[i];
            c->coeff_state[i] = 1;
            if (threshold > 0 && threshold < 32768) {
                is_null = 0;
                c->coeff_state[i] = 0;
            }
        }
        return is_null;
    }
    if (c->quant_high[band] <= 0 || c->quant_high[band] >= 32768) return 1;
    for (i = 0; i < (band_size[band] << 4); ++i) c->coeff_state[i] = 0;
    return 0;
}

static void decode_buckets(IwCodec *c, QwDjvuZp *zp, int band, IwBlock *block,
                           int fbucket, int nbucket)
{
    int thres = c->quant_high[band];
    int bbstate = 0;
    int16_t *pbuck[16];
    int cidx = 0;
    int buckno;
    int i;
    const int band0 = (band == 0);

    for (buckno = 0; buckno < nbucket; ++buckno)
        pbuck[buckno] = block->buckets[fbucket + buckno];

    for (buckno = 0; buckno < nbucket; ++buckno, cidx += 16) {
        int bstatetmp = 0;
        int16_t *pcoeff = pbuck[buckno];
        if (pcoeff == NULL) {
            bstatetmp = 8;
        } else if (!band0) {
            for (i = 0; i < 16; ++i) {
                int cs = pcoeff[i] != 0 ? 2 : 8;
                c->coeff_state[cidx + i] = (int8_t)cs;
                bstatetmp |= cs;
            }
        } else {
            for (i = 0; i < 16; ++i) {
                int cs = c->coeff_state[cidx + i] & 1;
                if (cs == 0) cs |= pcoeff[i] != 0 ? 2 : 8;
                c->coeff_state[cidx + i] = (int8_t)cs;
                bstatetmp |= cs;
            }
        }
        c->bucket_state[buckno] = (int8_t)bstatetmp;
        bbstate |= bstatetmp;
    }

    if (nbucket < 16 || (bbstate & 2) != 0) {
        bbstate |= 4;
    } else if ((bbstate & 8) != 0) {
        if (qw_djvu_zp_decode(zp, &c->ctx_root) != 0) bbstate |= 4;
    }

    if ((bbstate & 4) != 0) {
        for (buckno = 0; buckno < nbucket; ++buckno) {
            if ((c->bucket_state[buckno] & 8) != 0) {
                int ctx = 0;
                if (!band0) {
                    int k = (fbucket + buckno) << 2;
                    int16_t *b = block->buckets[k >> 4];
                    if (b != NULL) {
                        k &= 0xf;
                        ctx = (b[k] != 0) + (b[k + 1] != 0) +
                              (b[k + 2] != 0) + (b[k + 3] != 0);
                        if (ctx > 3) ctx = 3;
                    }
                }
                if ((bbstate & 2) != 0) ctx |= 4;
                if (qw_djvu_zp_decode(zp, &c->ctx_bucket[band][ctx]) != 0)
                    c->bucket_state[buckno] |= 4;
            }
        }
    }

    if ((bbstate & 4) != 0) {
        cidx = 0;
        for (buckno = 0; buckno < nbucket; ++buckno, cidx += 16) {
            if ((c->bucket_state[buckno] & 4) != 0) {
                int16_t *pcoeff = pbuck[buckno];
                int gotcha = 0;
                const int maxgotcha = 7;
                if (pcoeff == NULL) {
                    pcoeff = bucket_get(block, fbucket + buckno);
                    pbuck[buckno] = pcoeff;
                    if (!pcoeff) return;
                    for (i = 0; i < 16; ++i)
                        if ((c->coeff_state[cidx + i] & 1) == 0)
                            c->coeff_state[cidx + i] = 8;
                }
                for (i = 0; i < 16; ++i)
                    if ((c->coeff_state[cidx + i] & 8) != 0) ++gotcha;
                for (i = 0; i < 16; ++i) {
                    if ((c->coeff_state[cidx + i] & 8) != 0) {
                        int ctx;
                        int coeff;
                        int halfthres;
                        int t = band0 ? c->quant_low[i] : thres;
                        ctx = gotcha >= maxgotcha ? maxgotcha : gotcha;
                        if ((c->bucket_state[buckno] & 2) != 0) ctx |= 8;
                        if (qw_djvu_zp_decode(zp, &c->ctx_start[ctx]) != 0) {
                            c->coeff_state[cidx + i] |= 4;
                            halfthres = t >> 1;
                            coeff = (t + halfthres) - (halfthres >> 2);
                            {
                                int neg = -qw_djvu_zp_decode_iw(zp);
                                pcoeff[i] = (int16_t)((coeff ^ neg) - neg);
                            }
                        }
                        if ((c->coeff_state[cidx + i] & 4) != 0) gotcha = 0;
                        else if (gotcha > 0) --gotcha;
                    }
                }
            }
        }
    }

    if ((bbstate & 2) != 0) {
        cidx = 0;
        for (buckno = 0; buckno < nbucket; ++buckno, cidx += 16) {
            if ((c->bucket_state[buckno] & 2) != 0) {
                int16_t *pcoeff = pbuck[buckno];
                for (i = 0; i < 16; ++i) {
                    if ((c->coeff_state[cidx + i] & 2) != 0) {
                        int coeff = pcoeff[i];
                        int t = band0 ? c->quant_low[i] : thres;
                        if (coeff < 0) coeff = -coeff;
                        if (coeff <= 3 * t) {
                            coeff += t >> 2;
                            if (qw_djvu_zp_decode(zp, &c->ctx_mant) != 0)
                                coeff += t >> 1;
                            else
                                coeff = (coeff - t) + (t >> 1);
                        } else {
                            if (qw_djvu_zp_decode_iw(zp) != 0)
                                coeff += t >> 1;
                            else
                                coeff = (coeff - t) + (t >> 1);
                        }
                        {
                            int neg = -(pcoeff[i] < 0);
                            pcoeff[i] = (int16_t)((coeff ^ neg) - neg);
                        }
                    }
                }
            }
        }
    }
}

/* Decode one slice; returns 1 when more slices follow, 0 when the plane is
 * complete. */
static int code_slice(IwCodec *c, IwMap *map, QwDjvuZp *zp)
{
    if (c->curbit < 0) return 0;
    if (!is_null_slice(c, c->curband)) {
        int fbucket = band_start[c->curband];
        int nbucket = band_size[c->curband];
        int blockno;
        for (blockno = 0; blockno < map->nb; ++blockno) {
            decode_buckets(c, zp, c->curband, &map->blocks[blockno], fbucket,
                           nbucket);
        }
    }
    if (++c->curband >= 10) {
        c->curband = 0;
        ++c->curbit;
        if (next_quant(c) == 0) {
            c->curbit = -1;
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Inverse wavelet transform                                           */
/* ------------------------------------------------------------------ */

static void filter_bv(int16_t *p, int w, int h, int rowsize, int scale)
{
    int y = 0;
    int s = scale * rowsize;
    int s3 = s + s + s;
    int rows = ((h - 1) / scale) + 1;

    while (y - 3 < rows) {
        {
            int16_t *q = p;
            int16_t *e = q + w;
            if (y >= 3 && y + 3 < rows) {
                while (q < e) {
                    int a = (int)q[-s] + (int)q[s];
                    int b = (int)q[-s3] + (int)q[s3];
                    *q = (int16_t)(*q - (((a * 8) + a - b + 16) >> 5));
                    q += scale;
                }
            } else if (y < rows) {
                int16_t *q1 = (y + 1 < rows) ? q + s : NULL;
                int16_t *q3 = (y + 3 < rows) ? q + s3 : NULL;
                if (y >= 3) {
                    while (q < e) {
                        int a = (int)q[-s] + (q1 ? (int)*q1 : 0);
                        int b = (int)q[-s3] + (q3 ? (int)*q3 : 0);
                        *q = (int16_t)(*q - (((a * 8) + a - b + 16) >> 5));
                        q += scale;
                        if (q1) q1 += scale;
                        if (q3) q3 += scale;
                    }
                } else if (y >= 1) {
                    while (q < e) {
                        int a = (int)q[-s] + (q1 ? (int)*q1 : 0);
                        int b = (q3 ? (int)*q3 : 0);
                        *q = (int16_t)(*q - (((a * 8) + a - b + 16) >> 5));
                        q += scale;
                        if (q1) q1 += scale;
                        if (q3) q3 += scale;
                    }
                } else {
                    while (q < e) {
                        int a = q1 ? (int)*q1 : 0;
                        int b = q3 ? (int)*q3 : 0;
                        *q = (int16_t)(*q - (((a * 8) + a - b + 16) >> 5));
                        q += scale;
                        if (q1) q1 += scale;
                        if (q3) q3 += scale;
                    }
                }
            }
        }
        {
            int16_t *q = p - s3;
            int16_t *e = q + w;
            if (y >= 6 && y < rows) {
                while (q < e) {
                    int a = (int)q[-s] + (int)q[s];
                    int b = (int)q[-s3] + (int)q[s3];
                    *q = (int16_t)(*q + (((a * 8) + a - b + 8) >> 4));
                    q += scale;
                }
            } else if (y >= 3) {
                int16_t *q1 = (y - 2 < rows) ? q + s : q - s;
                while (q < e) {
                    int a = (int)q[-s] + (int)*q1;
                    *q = (int16_t)(*q + ((a + 1) >> 1));
                    q += scale;
                    q1 += scale;
                }
            }
        }
        y += 2;
        p += s + s;
    }
}

static void filter_bh_row(int16_t *p, int w, int s, int s3)
{
    int16_t *q = p;
    int16_t *e = p + w;
    int a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int b0 = 0, b1 = 0, b2 = 0, b3 = 0;

    if (q < e) {
        if (q + s < e) a2 = q[s];
        if (q + s3 < e) a3 = q[s3];
        b2 = b3 = q[0] - ((((a1 + a2) * 8) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q += s + s;
    }
    if (q < e) {
        a0 = a1;
        a1 = a2;
        a2 = a3;
        if (q + s3 < e) a3 = q[s3];
        b3 = q[0] - ((((a1 + a2) * 8) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q += s + s;
    }
    if (q < e) {
        b1 = b2;
        b2 = b3;
        a0 = a1;
        a1 = a2;
        a2 = a3;
        if (q + s3 < e) a3 = q[s3];
        b3 = q[0] - ((((a1 + a2) * 8) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q[-s3] = (int16_t)(q[-s3] + ((b1 + b2 + 1) >> 1));
        q += s + s;
    }
    while (q + s3 < e) {
        a0 = a1;
        a1 = a2;
        a2 = a3;
        a3 = q[s3];
        b0 = b1;
        b1 = b2;
        b2 = b3;
        b3 = q[0] - ((((a1 + a2) * 8) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q[-s3] = (int16_t)(q[-s3] +
                           ((((b1 + b2) * 8) + (b1 + b2) - b0 - b3 + 8) >> 4));
        q += s + s;
    }
    while (q < e) {
        a0 = a1;
        a1 = a2;
        a2 = a3;
        a3 = 0;
        b0 = b1;
        b1 = b2;
        b2 = b3;
        b3 = q[0] - ((((a1 + a2) * 8) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q[-s3] = (int16_t)(q[-s3] +
                           ((((b1 + b2) * 8) + (b1 + b2) - b0 - b3 + 8) >> 4));
        q += s + s;
    }
    while (q - s3 < e) {
        b0 = b1;
        b1 = b2;
        b2 = b3;
        if (q - s3 >= p) q[-s3] = (int16_t)(q[-s3] + ((b1 + b2 + 1) >> 1));
        q += s + s;
    }
}

static void filter_bh(int16_t *p, int w, int h, int rowsize, int scale)
{
    int y = 0;
    int s = scale;
    int s3 = s + s + s;
    int step = scale * rowsize;

    while (y < h) {
        filter_bh_row(p, w, s, s3);
        y += scale;
        p += step;
    }
}

static void backward(int16_t *p, int w, int h, int rowsize, int begin, int end)
{
    int scale;
    for (scale = begin >> 1; scale >= end; scale >>= 1) {
        filter_bv(p, w, h, rowsize, scale);
        filter_bh(p, w, h, rowsize, scale);
    }
}

/* ------------------------------------------------------------------ */
/* Coefficient scatter and pixel reconstruction                        */
/* ------------------------------------------------------------------ */

static int16_t *build_unified(const IwMap *map)
{
    size_t count = (size_t)map->bw * (size_t)map->bh +
                   (size_t)map->bw * 4u + 16u;
    int16_t *plane = (int16_t *)calloc(count, sizeof(int16_t));
    int blockrow;
    int blockcol;
    int blockidx = 0;

    if (!plane) return NULL;
    for (blockrow = 0; blockrow < map->bh; blockrow += 32) {
        for (blockcol = 0; blockcol < map->bw; blockcol += 32) {
            const IwBlock *block = &map->blocks[blockidx++];
            int base = blockrow * map->bw + blockcol;
            int n = 0;
            int b1, b2;
            for (b1 = 0; b1 < 64; ++b1) {
                const int16_t *bucket = block->buckets[b1];
                if (bucket) {
                    for (b2 = 0; b2 < 16; ++b2, ++n) {
                        int zi = qw_djvu_iw44_zigzag[n];
                        plane[base + (zi >> 5) * map->bw + (zi & 31)] =
                            bucket[b2];
                    }
                } else {
                    n += 16;
                }
            }
        }
    }
    return plane;
}

static int map_image(const IwMap *map, int8_t *out, int fast)
{
    int16_t *plane = build_unified(map);
    int i;
    int j;

    if (!plane) return -1;
    if (fast) {
        int pidx = 0;
        backward(plane, map->w, map->h, map->bw, 32, 2);
        for (i = 0; i < map->bh; i += 2, pidx += map->bw) {
            for (j = 0; j < map->bw; j += 2, pidx += 2) {
                plane[pidx + map->bw] = plane[pidx];
                plane[pidx + map->bw + 1] = plane[pidx];
                plane[pidx + 1] = plane[pidx];
            }
        }
    } else {
        backward(plane, map->w, map->h, map->bw, 32, 1);
    }

    for (i = 0; i < map->h; ++i) {
        const int16_t *row = plane + (size_t)i * (size_t)map->bw;
        for (j = 0; j < map->w; ++j) {
            int v = ((int)row[j] + 32) >> 6;
            if (v < -128) v = -128;
            else if (v > 127) v = 127;
            out[(size_t)i * (size_t)map->w + (size_t)j] = (int8_t)v;
        }
    }
    free(plane);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Chunk decoding                                                      */
/* ------------------------------------------------------------------ */

static int decode_chunk(struct QwDjvuIw44Image *image, const uint8_t *data,
                        size_t size)
{
    QwDjvuZp zp;
    size_t pos = 0;
    int serial;
    int slices;
    int nslices;
    int flag;

    if (size < 2) return -1;
    serial = data[pos++];
    slices = data[pos++];
    if (serial != image->serial) return -1;
    nslices = image->cslices + slices;

    if (image->serial == 0) {
        int major;
        int minor;
        int w;
        int h;
        int crcbdelay = 0;
        if (pos + 6 > size) return -1;
        major = data[pos++];
        minor = data[pos++];
        if ((major & 0x7f) != 1) return -1;
        w = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        h = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        if (minor >= 2) {
            if (pos >= size) return -1;
            crcbdelay = data[pos++];
            image->crcbdelay = crcbdelay & 0x7f;
        }
        if (minor >= 2)
            image->crcbhalf = (crcbdelay & 0x80) ? 0 : 1;
        if (major & 0x80) image->crcbdelay = -1;

        image->w = w;
        image->h = h;
        image->ymap = map_new(w, h);
        if (!image->ymap) return -1;
        codec_init(&image->yc);
        if (image->crcbdelay >= 0) {
            image->cbmap = map_new(w, h);
            image->crmap = map_new(w, h);
            if (!image->cbmap || !image->crmap) return -1;
            codec_init(&image->cbc);
            codec_init(&image->crc);
            image->has_color = 1;
        }
    }

    qw_djvu_zp_init(&zp, data + pos, size - pos);

    for (flag = 1; flag != 0 && image->cslices < nslices; ++image->cslices) {
        flag = code_slice(&image->yc, image->ymap, &zp);
        if (image->has_color && image->crcbdelay <= image->cslices) {
            flag |= code_slice(&image->cbc, image->cbmap, &zp);
            flag |= code_slice(&image->crc, image->crmap, &zp);
        }
    }
    ++image->serial;
    return 0;
}

int qw_djvu_iw44_decode_chunks(struct QwDjvuIw44Image **out_image,
                               const uint8_t *const *chunks,
                               const size_t *sizes, size_t count)
{
    struct QwDjvuIw44Image *image;
    size_t i;

    if (!out_image) return -1;
    *out_image = NULL;
    if (!chunks || !sizes || count == 0) return -1;

    image = (struct QwDjvuIw44Image *)calloc(1, sizeof(*image));
    if (!image) return -1;

    for (i = 0; i < count; ++i) {
        if (decode_chunk(image, chunks[i], sizes[i]) != 0) {
            qw_djvu_iw44_free(image);
            return -1;
        }
    }
    if (!image->ymap || image->w <= 0 || image->h <= 0) {
        qw_djvu_iw44_free(image);
        return -1;
    }
    *out_image = image;
    return 0;
}

void qw_djvu_iw44_free(struct QwDjvuIw44Image *image)
{
    if (!image) return;
    map_free(image->ymap);
    map_free(image->cbmap);
    map_free(image->crmap);
    free(image);
}

int qw_djvu_iw44_size(const struct QwDjvuIw44Image *image, int *width,
                      int *height)
{
    if (!image) return -1;
    if (width) *width = image->w;
    if (height) *height = image->h;
    return 0;
}

int qw_djvu_iw44_is_color(const struct QwDjvuIw44Image *image)
{
    return image && image->has_color;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

int qw_djvu_iw44_render_rgb(const struct QwDjvuIw44Image *image, uint8_t *rgb)
{
    size_t plane;
    int8_t *planes;
    int row;

    if (!image || !rgb || !image->ymap) return -1;
    plane = (size_t)image->w * (size_t)image->h;
    planes = (int8_t *)calloc(plane * (image->has_color ? 3u : 1u), 1u);
    if (!planes) return -1;

    if (map_image(image->ymap, planes, 0) != 0) {
        free(planes);
        return -1;
    }
    if (image->has_color) {
        if (map_image(image->cbmap, planes + plane, image->crcbhalf) != 0 ||
            map_image(image->crmap, planes + 2 * plane, image->crcbhalf) != 0) {
            free(planes);
            return -1;
        }
    }

    for (row = 0; row < image->h; ++row) {
        int src = image->h - 1 - row;
        const int8_t *y = planes + (size_t)src * (size_t)image->w;
        uint8_t *dst = rgb + (size_t)row * (size_t)image->w * 3u;
        if (image->has_color) {
            const int8_t *b = planes + plane + (size_t)src * (size_t)image->w;
            const int8_t *r = planes + 2 * plane +
                              (size_t)src * (size_t)image->w;
            int x;
            for (x = 0; x < image->w; ++x) {
                int yv = y[x];
                int bv = b[x];
                int rv = r[x];
                int t1 = bv >> 2;
                int t2 = rv + (rv >> 1);
                int t3 = yv + 128 - t1;
                int tr = yv + 128 + t2;
                int tg = t3 - (t2 >> 1);
                int tb = t3 + (bv * 2);
                dst[0] = (uint8_t)clamp255(tr);
                dst[1] = (uint8_t)clamp255(tg);
                dst[2] = (uint8_t)clamp255(tb);
                dst += 3;
            }
        } else {
            int x;
            for (x = 0; x < image->w; ++x) {
                uint8_t g = (uint8_t)clamp255(127 - y[x]);
                dst[0] = g;
                dst[1] = g;
                dst[2] = g;
                dst += 3;
            }
        }
    }
    free(planes);
    return 0;
}

int qw_djvu_iw44_render_gray(const struct QwDjvuIw44Image *image, uint8_t *gray)
{
    size_t plane;
    int8_t *y = NULL;
    int row;

    if (!image || !gray || !image->ymap) return -1;
    plane = (size_t)image->w * (size_t)image->h;
    y = (int8_t *)calloc(plane, 1u);
    if (!y) return -1;
    if (map_image(image->ymap, y, 0) != 0) {
        free(y);
        return -1;
    }
    for (row = 0; row < image->h; ++row) {
        int src = image->h - 1 - row;
        const int8_t *s = y + (size_t)src * (size_t)image->w;
        uint8_t *dst = gray + (size_t)row * (size_t)image->w;
        int x;
        if (image->has_color) {
            for (x = 0; x < image->w; ++x) dst[x] = (uint8_t)clamp255(127 - s[x]);
        } else {
            for (x = 0; x < image->w; ++x) dst[x] = (uint8_t)clamp255(127 - s[x]);
        }
    }
    free(y);
    return 0;
}

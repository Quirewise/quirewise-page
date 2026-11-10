/* First-party DjVu ZP arithmetic decoder (decode side only). */

#include "djvu_zp.h"

#include <string.h>

static int zp_read_byte(QwDjvuZp *zp)
{
    if (zp->pos < zp->size) return (int)zp->data[zp->pos++];
    return -1;
}

static void zp_preload(QwDjvuZp *zp)
{
    unsigned scount = zp->scount;
    int zbyte = zp->zbyte;
    uint32_t buffer = zp->buffer;

    while (scount <= 24u) {
        int b = zp_read_byte(zp);
        if (b < 0) {
            zbyte = 0xff;
            if (--zp->delay < 1) zp->eof = 1;
        } else {
            zbyte = b;
        }
        buffer = (buffer << 8) | (uint32_t)(uint8_t)zbyte;
        scount += 8u;
    }
    zp->scount = (uint8_t)scount;
    zp->zbyte = (uint8_t)zbyte;
    zp->buffer = buffer;
}

/* Number of leading one bits in a byte. */
static int zp_leading_ones8(uint32_t byte)
{
    int n = 0;
    uint32_t mask = 0x80u;
    while (n < 8 && (byte & mask) != 0u) {
        mask >>= 1;
        ++n;
    }
    return n;
}

/* Number of leading one bits in the ZP renormalisation value.
 *
 * The value is normally below 0x10000; the >= 0xff00 branch mirrors the
 * format's two-byte shortcut so that both bytes-0xff and overflow values
 * normalise exactly as the reference coder does. */
static int zp_ffz(uint32_t x)
{
    if (x < 0xff00u) return zp_leading_ones8((x >> 8) & 0xffu);
    return 8 + zp_leading_ones8(x & 0xffu);
}

static uint32_t zp_buffer_bits(const QwDjvuZp *zp, int count)
{
    uint32_t mask;
    if (count <= 0) return 0u;
    if (count >= 32) return zp->buffer;
    mask = (1u << (unsigned)count) - 1u;
    if (zp->scount >= 32u) return 0u;
    return (zp->buffer >> zp->scount) & mask;
}

static void zp_renorm_lps(QwDjvuZp *zp, uint32_t z)
{
    int shift;

    z = 0x10000u - z;
    zp->a += z;
    zp->code += z;
    shift = zp_ffz(zp->a);
    zp->scount = (uint8_t)(zp->scount - (unsigned)shift);
    zp->a = (zp->a << (unsigned)shift) & 0xffffu;
    zp->code = ((zp->code << (unsigned)shift) & 0xffffu) |
               zp_buffer_bits(zp, shift);
    if (zp->scount < 16u) zp_preload(zp);
    zp->fence = zp->code;
    if (zp->code >= 0x8000u) zp->fence = 0x7fffu;
}

static void zp_renorm_mps(QwDjvuZp *zp, uint32_t z)
{
    zp->scount = (uint8_t)(zp->scount - 1u);
    zp->a = (z << 1) & 0xffffu;
    zp->code = ((zp->code << 1) & 0xffffu) |
               ((zp->scount >= 32u) ? 0u
                                    : ((zp->buffer >> zp->scount) & 1u));
    if (zp->scount < 16u) zp_preload(zp);
    zp->fence = zp->code;
    if (zp->code >= 0x8000u) zp->fence = 0x7fffu;
}

static int zp_decode_slow(QwDjvuZp *zp, uint8_t *ctx, uint32_t z)
{
    int bit = (int)(*ctx & 1u);
    uint32_t d = 0x6000u + ((z + zp->a) >> 2);

    if (z > d) z = d;
    if (z > zp->code) {
        *ctx = zp->dn[*ctx];
        zp_renorm_lps(zp, z);
        return bit ^ 1;
    }
    if (zp->a >= zp->m[*ctx]) *ctx = zp->up[*ctx];
    zp_renorm_mps(zp, z);
    return bit;
}

int qw_djvu_zp_decode(QwDjvuZp *zp, uint8_t *ctx)
{
    uint32_t z = zp->a + zp->p[*ctx];
    if (z <= zp->fence) {
        zp->a = z;
        return (int)(*ctx & 1u);
    }
    return zp_decode_slow(zp, ctx, z);
}

static int zp_decode_simple(QwDjvuZp *zp, int mps, uint32_t z)
{
    if (z > zp->code) {
        zp_renorm_lps(zp, z);
        return mps ^ 1;
    }
    zp_renorm_mps(zp, z);
    return mps;
}

int qw_djvu_zp_decode_pass(QwDjvuZp *zp)
{
    return zp_decode_simple(zp, 0, 0x8000u + (zp->a >> 1));
}

int qw_djvu_zp_decode_iw(QwDjvuZp *zp)
{
    uint32_t z = 0x8000u + ((zp->a + zp->a + zp->a) >> 3);
    return zp_decode_simple(zp, 0, z);
}

void qw_djvu_zp_init(QwDjvuZp *zp, const uint8_t *data, size_t size)
{
    int i;
    int b;

    memset(zp, 0, sizeof(*zp));
    zp->data = data;
    zp->size = size;

    for (i = 0; i < 256; ++i) {
        zp->p[i] = qw_djvu_zp_default_table[i].p;
        zp->m[i] = qw_djvu_zp_default_table[i].m;
        zp->up[i] = qw_djvu_zp_default_table[i].up;
        zp->dn[i] = qw_djvu_zp_default_table[i].dn;
    }

    zp->code = 0xff00u;
    b = zp_read_byte(zp);
    zp->code = 0xff00u & ((uint32_t)b << 8);
    b = zp_read_byte(zp);
    zp->zbyte = (uint8_t)(0xffu & (uint32_t)b);
    zp->code |= (uint32_t)zp->zbyte;
    zp->delay = 25u;
    zp->scount = 0u;
    zp_preload(zp);
    zp->fence = zp->code;
    if (zp->code >= 0x8000u) zp->fence = 0x7fffu;
}

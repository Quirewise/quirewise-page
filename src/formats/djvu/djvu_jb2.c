/* First-party DjVu JB2 bitonal decoder implementation.
 *
 * The record model follows the DjVu specification: StartOfData, NewMark,
 * NewMarkLibraryOnly, NewMarkImageOnly, MatchedRefine(+library/image only),
 * MatchedCopy, NonMarkData, PreservedComment, RequiredDictOrReset and
 * EndOfData. Shape bitmaps are coded either directly (5x10 pixel context) or
 * cross-coded against a library reference shape; both use the ZP coder from
 * djvu_zp.c. */

#include "djvu_jb2.h"
#include "djvu_zp.h"

#include <stdlib.h>
#include <string.h>

#define QW_JB2_MAX_SHAPES 65536
#define QW_JB2_MAX_BLITS (1 << 20)
#define QW_JB2_MAX_PIXELS ((size_t)100u * 1000u * 1000u)
#define QW_JB2_MAX_DIM 65535

enum {
    REC_StartOfData = 0,
    REC_NewMark = 1,
    REC_NewMarkLibraryOnly = 2,
    REC_NewMarkImageOnly = 3,
    REC_MatchedRefine = 4,
    REC_MatchedRefineLibraryOnly = 5,
    REC_MatchedRefineImageOnly = 6,
    REC_MatchedCopy = 7,
    REC_NonMarkData = 8,
    REC_RequiredDictOrReset = 9,
    REC_PreservedComment = 10,
    REC_EndOfData = 11
};

#define QW_JB2_BIGPOS 262142
#define QW_JB2_BIGNEG (-262143)

typedef struct QwDjvuJb2Bitmap {
    int width;
    int height;
    int border;
    int stride;
    uint8_t *data;
    uint8_t *guard;
} QwDjvuJb2Bitmap;

typedef struct QwDjvuJb2Shape {
    int parent;
    QwDjvuJb2Bitmap bm;
} QwDjvuJb2Shape;

typedef struct QwDjvuJb2Blit {
    int left;
    int bottom;
    int shapeno;
} QwDjvuJb2Blit;

typedef struct QwDjvuJb2Image {
    int width;
    int height;
    int inherited_shapes;
    QwDjvuJb2Shape *shapes;
    int nshapes;
    int cap_shapes;
    QwDjvuJb2Blit *blits;
    int nblits;
    int cap_blits;
} QwDjvuJb2Image;

typedef struct QwJb2Codec {
    QwDjvuZp zp;

    uint8_t *cells;
    int *left;
    int *right;
    int ncells;
    int cap_cells;

    int dist_record_type;
    int dist_match_index;
    int dist_abs_loc_x;
    int dist_abs_loc_y;
    int dist_abs_size_x;
    int dist_abs_size_y;
    int dist_image_size;
    int dist_inherited_count;
    int dist_rel_loc_x_cur;
    int dist_rel_loc_x_last;
    int dist_rel_loc_y_cur;
    int dist_rel_loc_y_last;
    int dist_rel_size_x;
    int dist_rel_size_y;
    int dist_comment_byte;
    int dist_comment_length;
    uint8_t dist_offset_type;
    uint8_t dist_refinement_flag;

    uint8_t bitdist[1024];
    uint8_t cbitdist[2048];

    int *lib2shape;
    int nlib2shape;
    int cap_lib2shape;
    int *shape2lib;
    int nshape2lib;
    int cap_shape2lib;
    int *libinfo; /* 4 ints per library: xmin, ymin, xmax, ymax */
    int nlibinfo;
    int cap_libinfo;

    int shortlist[3];
    int shortlistpos;
    int last_left;
    int last_right;
    int last_bottom;
    int last_row_left;
    int last_row_bottom;
    int image_columns;
    int image_rows;
    int got_start_record;
    int refinement_flag;
    int error;
} QwJb2Codec;

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                      */
/* ------------------------------------------------------------------ */

static int bm_init(QwDjvuJb2Bitmap *bm, int width, int height)
{
    size_t stride;
    size_t total;

    if (!bm) return -1;
    if (width < 0 || height < 0) return -1;
    if (width > QW_JB2_MAX_DIM || height > QW_JB2_MAX_DIM) return -1;
    memset(bm, 0, sizeof(*bm));
    bm->width = width;
    bm->height = height;
    bm->border = 4;
    stride = (size_t)width + 2u * (size_t)bm->border;
    bm->stride = (int)stride;
    total = ((size_t)height + 1u) * stride;
    bm->data = (uint8_t *)calloc(total ? total : 1u, 1u);
    if (!bm->data) return -1;
    bm->guard = bm->data + (size_t)height * stride;
    return 0;
}

static void bm_free(QwDjvuJb2Bitmap *bm)
{
    if (!bm) return;
    free(bm->data);
    memset(bm, 0, sizeof(*bm));
}

static uint8_t *bm_row(const QwDjvuJb2Bitmap *bm, int row)
{
    if (row < 0 || row >= bm->height) return bm->guard + bm->border;
    return bm->data + (size_t)row * (size_t)bm->stride + (size_t)bm->border;
}

static int bm_pixel(const QwDjvuJb2Bitmap *bm, int x, int y)
{
    if (!bm || x < 0 || y < 0 || x >= bm->width || y >= bm->height) return 0;
    return bm->data[(size_t)y * (size_t)bm->stride + (size_t)bm->border +
                    (size_t)x] != 0;
}

static void bm_bbox(const QwDjvuJb2Bitmap *bm, int *xmin, int *ymin, int *xmax,
                    int *ymax)
{
    int x, y;
    int xa = 0, xb = -1, ya = 0, yb = -1;

    for (xb = bm->width - 1; xb >= 0; --xb) {
        int found = 0;
        for (y = 0; y < bm->height && !found; ++y) {
            if (bm_pixel(bm, xb, y)) found = 1;
        }
        if (found) break;
    }
    for (yb = bm->height - 1; yb >= 0; --yb) {
        int found = 0;
        for (x = 0; x < bm->width && !found; ++x) {
            if (bm_pixel(bm, x, yb)) found = 1;
        }
        if (found) break;
    }
    for (xa = 0; xa <= xb; ++xa) {
        int found = 0;
        for (y = 0; y < bm->height && !found; ++y) {
            if (bm_pixel(bm, xa, y)) found = 1;
        }
        if (found) break;
    }
    for (ya = 0; ya <= yb; ++ya) {
        int found = 0;
        for (x = 0; x < bm->width && !found; ++x) {
            if (bm_pixel(bm, x, ya)) found = 1;
        }
        if (found) break;
    }
    *xmin = xa;
    *ymin = ya;
    *xmax = xb;
    *ymax = yb;
}

/* ------------------------------------------------------------------ */
/* Image shape/blit storage                                            */
/* ------------------------------------------------------------------ */

static int image_add_shape(QwDjvuJb2Image *img, QwDjvuJb2Shape **out)
{
    QwDjvuJb2Shape *grown;
    if (img->nshapes >= QW_JB2_MAX_SHAPES) return -1;
    if (img->nshapes >= img->cap_shapes) {
        int cap = img->cap_shapes ? img->cap_shapes * 2 : 16;
        grown = (QwDjvuJb2Shape *)realloc(img->shapes,
                                          sizeof(QwDjvuJb2Shape) * (size_t)cap);
        if (!grown) return -1;
        img->shapes = grown;
        img->cap_shapes = cap;
    }
    memset(&img->shapes[img->nshapes], 0, sizeof(QwDjvuJb2Shape));
    *out = &img->shapes[img->nshapes];
    ++img->nshapes;
    return 0;
}

static int image_add_blit(QwDjvuJb2Image *img, QwDjvuJb2Blit blit)
{
    QwDjvuJb2Blit *grown;
    if (img->nblits >= QW_JB2_MAX_BLITS) return -1;
    if (img->nblits >= img->cap_blits) {
        int cap = img->cap_blits ? img->cap_blits * 2 : 64;
        grown = (QwDjvuJb2Blit *)realloc(img->blits,
                                         sizeof(QwDjvuJb2Blit) * (size_t)cap);
        if (!grown) return -1;
        img->blits = grown;
        img->cap_blits = cap;
    }
    img->blits[img->nblits++] = blit;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Number coder                                                        */
/* ------------------------------------------------------------------ */

static int ensure_cells(QwJb2Codec *c, int need)
{
    int cap;
    uint8_t *cells;
    int *left;
    int *right;

    if (need <= c->cap_cells) return 0;
    cap = c->cap_cells ? c->cap_cells : 1024;
    while (cap < need) cap *= 2;
    cells = (uint8_t *)calloc((size_t)cap, 1u);
    left = (int *)calloc((size_t)cap, sizeof(int));
    right = (int *)calloc((size_t)cap, sizeof(int));
    if (!cells || !left || !right) {
        free(cells);
        free(left);
        free(right);
        return -1;
    }
    if (c->cells && c->ncells > 0) {
        memcpy(cells, c->cells, (size_t)c->ncells);
        memcpy(left, c->left, sizeof(int) * (size_t)c->ncells);
        memcpy(right, c->right, sizeof(int) * (size_t)c->ncells);
    }
    free(c->cells);
    free(c->left);
    free(c->right);
    c->cells = cells;
    c->left = left;
    c->right = right;
    c->cap_cells = cap;
    return 0;
}

static int code_num(QwJb2Codec *c, int low, int high, int *root)
{
    int negative = 0;
    int cutoff = 0;
    int phase = 1;
    int range = -1;
    int *ctx = root;

    if (ensure_cells(c, c->ncells + 128) != 0) {
        c->error = 1;
        return 0;
    }

    while (range != 1) {
        int cell = *ctx;
        int decision;
        if (cell == 0) {
            cell = c->ncells;
            *ctx = cell;
            c->cells[cell] = 0;
            c->left[cell] = 0;
            c->right[cell] = 0;
            ++c->ncells;
        }
        decision = (low >= cutoff) ||
                   ((high >= cutoff) &&
                    qw_djvu_zp_decode(&c->zp, &c->cells[cell]) != 0);
        ctx = decision ? &c->right[cell] : &c->left[cell];

        switch (phase) {
        case 1:
            negative = !decision;
            if (negative) {
                int t = -low - 1;
                low = -high - 1;
                high = t;
            }
            phase = 2;
            cutoff = 1;
            break;
        case 2:
            if (!decision) {
                phase = 3;
                range = (cutoff + 1) >> 1;
                if (range == 1) cutoff = 0;
                else cutoff -= (range >> 1);
            } else {
                cutoff = (cutoff << 1) + 1;
            }
            break;
        case 3:
            range /= 2;
            if (range != 1) {
                if (!decision) cutoff -= (range >> 1);
                else cutoff += (range >> 1);
            } else if (!decision) {
                --cutoff;
            }
            break;
        default:
            break;
        }
    }
    return negative ? (-cutoff - 1) : cutoff;
}

/* ------------------------------------------------------------------ */
/* Bitmap coders                                                       */
/* ------------------------------------------------------------------ */

static int direct_context(const uint8_t *up2, const uint8_t *up1,
                          const uint8_t *up0, int col)
{
    return (up2[col - 1] << 9) | (up2[col] << 8) | (up2[col + 1] << 7) |
           (up1[col - 2] << 6) | (up1[col - 1] << 5) | (up1[col] << 4) |
           (up1[col + 1] << 3) | (up1[col + 2] << 2) |
           (up0[col - 2] << 1) | (up0[col - 1]);
}

static void code_bitmap_direct(QwJb2Codec *c, QwDjvuJb2Bitmap *bm)
{
    int dy;
    for (dy = bm->height - 1; dy >= 0; --dy) {
        uint8_t *up0 = bm_row(bm, dy);
        uint8_t *up1 = bm_row(bm, dy + 1);
        uint8_t *up2 = bm_row(bm, dy + 2);
        int dx;
        for (dx = 0; dx < bm->width; ++dx) {
            int context = direct_context(up2, up1, up0, dx);
            int bit = qw_djvu_zp_decode(&c->zp, &c->bitdist[context]);
            up0[dx] = (uint8_t)bit;
        }
    }
}

static int ref_pixel(const QwDjvuJb2Bitmap *bm, int x, int y)
{
    if (!bm || x < 0 || y < 0 || x >= bm->width || y >= bm->height) return 0;
    return bm->data[(size_t)y * (size_t)bm->stride + (size_t)bm->border +
                    (size_t)x] != 0;
}

static int cross_context(const uint8_t *up1, const uint8_t *up0, int col,
                         const QwDjvuJb2Bitmap *ref, int rx, int ry)
{
    return (up1[col - 1] << 10) | (up1[col] << 9) | (up1[col + 1] << 8) |
           (up0[col - 1] << 7) |
           (ref_pixel(ref, rx, ry + 1) << 6) |
           (ref_pixel(ref, rx - 1, ry) << 5) |
           (ref_pixel(ref, rx, ry) << 4) |
           (ref_pixel(ref, rx + 1, ry) << 3) |
           (ref_pixel(ref, rx - 1, ry - 1) << 2) |
           (ref_pixel(ref, rx, ry - 1) << 1) |
           ref_pixel(ref, rx + 1, ry - 1);
}

static void code_bitmap_cross(QwJb2Codec *c, QwDjvuJb2Bitmap *bm,
                              const QwDjvuJb2Bitmap *ref, int xd2c, int yd2c)
{
    int dy;
    for (dy = bm->height - 1; dy >= 0; --dy) {
        uint8_t *up0 = bm_row(bm, dy);
        uint8_t *up1 = bm_row(bm, dy + 1);
        int ry = dy + yd2c;
        int dx;
        for (dx = 0; dx < bm->width; ++dx) {
            int context = cross_context(up1, up0, dx, ref, dx + xd2c, ry);
            int bit = qw_djvu_zp_decode(&c->zp, &c->cbitdist[context]);
            up0[dx] = (uint8_t)bit;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Library bookkeeping                                                 */
/* ------------------------------------------------------------------ */

static int iarr_push(int **arr, int *count, int *cap, int value)
{
    int *grown;
    if (*count >= *cap) {
        int newcap = *cap ? *cap * 2 : 32;
        grown = (int *)realloc(*arr, sizeof(int) * (size_t)newcap);
        if (!grown) return -1;
        *arr = grown;
        *cap = newcap;
    }
    (*arr)[(*count)++] = value;
    return 0;
}

static int set_shape2lib(QwJb2Codec *c, int shapeno, int libno)
{
    while (c->nshape2lib <= shapeno) {
        if (iarr_push(&c->shape2lib, &c->nshape2lib, &c->cap_shape2lib, -1) != 0)
            return -1;
    }
    c->shape2lib[shapeno] = libno;
    return 0;
}

static int add_library(QwJb2Codec *c, int shapeno, const QwDjvuJb2Shape *shape)
{
    int libno = c->nlib2shape;
    int xmin, ymin, xmax, ymax;

    if (iarr_push(&c->lib2shape, &c->nlib2shape, &c->cap_lib2shape, shapeno) != 0)
        return -1;
    if (set_shape2lib(c, shapeno, libno) != 0) return -1;
    bm_bbox(&shape->bm, &xmin, &ymin, &xmax, &ymax);
    if (iarr_push(&c->libinfo, &c->nlibinfo, &c->cap_libinfo, xmin) != 0 ||
        iarr_push(&c->libinfo, &c->nlibinfo, &c->cap_libinfo, ymin) != 0 ||
        iarr_push(&c->libinfo, &c->nlibinfo, &c->cap_libinfo, xmax) != 0 ||
        iarr_push(&c->libinfo, &c->nlibinfo, &c->cap_libinfo, ymax) != 0)
        return -1;
    return libno;
}

static int init_library(QwJb2Codec *c, QwDjvuJb2Image *img)
{
    c->nlib2shape = c->nshape2lib = c->nlibinfo = 0;
    /* Inherited dictionary shapes are not supported yet: the image decoder
       always decodes a self-contained Sjbz stream (inherited count is 0). */
    (void)img;
    return 0;
}

static void reset_numcoder(QwJb2Codec *c)
{
    c->dist_comment_byte = 0;
    c->dist_comment_length = 0;
    c->dist_record_type = 0;
    c->dist_match_index = 0;
    c->dist_abs_loc_x = 0;
    c->dist_abs_loc_y = 0;
    c->dist_abs_size_x = 0;
    c->dist_abs_size_y = 0;
    c->dist_image_size = 0;
    c->dist_inherited_count = 0;
    c->dist_rel_loc_x_cur = 0;
    c->dist_rel_loc_x_last = 0;
    c->dist_rel_loc_y_cur = 0;
    c->dist_rel_loc_y_last = 0;
    c->dist_rel_size_x = 0;
    c->dist_rel_size_y = 0;
    c->ncells = 1; /* index 0 stays reserved */
}

static void fill_shortlist(QwJb2Codec *c, int value)
{
    c->shortlist[0] = c->shortlist[1] = c->shortlist[2] = value;
    c->shortlistpos = 0;
}

static int update_shortlist(QwJb2Codec *c, int value)
{
    int *s = c->shortlist;
    if (++c->shortlistpos == 3) c->shortlistpos = 0;
    s[c->shortlistpos] = value;
    if (s[0] >= s[1]) {
        return (s[0] > s[2]) ? ((s[1] >= s[2]) ? s[1] : s[2]) : s[0];
    }
    return (s[0] < s[2]) ? ((s[1] >= s[2]) ? s[2] : s[1]) : s[0];
}

/* ------------------------------------------------------------------ */
/* Record level                                                        */
/* ------------------------------------------------------------------ */

static int code_record_type(QwJb2Codec *c)
{
    return code_num(c, REC_StartOfData, REC_EndOfData, &c->dist_record_type);
}

static int code_match_index(QwJb2Codec *c)
{
    int match;
    if (c->nlib2shape <= 0) return -1;
    match = code_num(c, 0, c->nlib2shape - 1, &c->dist_match_index);
    if (match < 0 || match >= c->nlib2shape) return -1;
    return match;
}

static int code_abs_mark_size(QwJb2Codec *c, QwDjvuJb2Bitmap *bm, int border)
{
    int xsize = code_num(c, 0, QW_JB2_BIGPOS, &c->dist_abs_size_x);
    int ysize = code_num(c, 0, QW_JB2_BIGPOS, &c->dist_abs_size_y);
    if (c->error) return -1;
    if (xsize != (0xffff & xsize) || ysize != (0xffff & ysize)) return -1;
    if (bm_init(bm, xsize, ysize) != 0) return -1;
    if ((size_t)xsize * (size_t)ysize > QW_JB2_MAX_PIXELS) return -1;
    (void)border;
    return 0;
}

static int code_rel_mark_size(QwJb2Codec *c, QwDjvuJb2Bitmap *bm, int cw,
                              int ch, int border)
{
    int xdiff = code_num(c, QW_JB2_BIGNEG, QW_JB2_BIGPOS, &c->dist_rel_size_x);
    int ydiff = code_num(c, QW_JB2_BIGNEG, QW_JB2_BIGPOS, &c->dist_rel_size_y);
    int xsize = cw + xdiff;
    int ysize = ch + ydiff;
    if (c->error) return -1;
    if (xsize != (0xffff & xsize) || ysize != (0xffff & ysize)) return -1;
    if (xsize <= 0 || ysize <= 0) return -1;
    if (bm_init(bm, xsize, ysize) != 0) return -1;
    if ((size_t)xsize * (size_t)ysize > QW_JB2_MAX_PIXELS) return -1;
    (void)border;
    return 0;
}

static void code_rel_location(QwJb2Codec *c, QwDjvuJb2Blit *blit, int rows,
                              int columns)
{
    int left = 0;
    int bottom = 0;
    int new_row = qw_djvu_zp_decode(&c->zp, &c->dist_offset_type);

    if (new_row) {
        int xdiff = code_num(c, QW_JB2_BIGNEG, QW_JB2_BIGPOS,
                             &c->dist_rel_loc_x_last);
        int ydiff = code_num(c, QW_JB2_BIGNEG, QW_JB2_BIGPOS,
                             &c->dist_rel_loc_y_last);
        int top;
        left = c->last_row_left + xdiff;
        top = c->last_row_bottom + ydiff;
        c->last_left = c->last_row_left = left;
        c->last_right = (left + columns) - 1;
        c->last_bottom = c->last_row_bottom = (top - rows) + 1;
        fill_shortlist(c, c->last_bottom);
        bottom = c->last_bottom;
    } else {
        int xdiff = code_num(c, QW_JB2_BIGNEG, QW_JB2_BIGPOS,
                             &c->dist_rel_loc_x_cur);
        int ydiff = code_num(c, QW_JB2_BIGNEG, QW_JB2_BIGPOS,
                             &c->dist_rel_loc_y_cur);
        left = c->last_right + xdiff;
        c->last_left = left;
        c->last_right = (left + columns) - 1;
        bottom = update_shortlist(c, c->last_bottom + ydiff);
        c->last_bottom = bottom;
    }
    blit->bottom = bottom - 1;
    blit->left = left - 1;
}

static void code_abs_location(QwJb2Codec *c, QwDjvuJb2Blit *blit, int rows)
{
    int left = code_num(c, 1, c->image_columns, &c->dist_abs_loc_x);
    int top = code_num(c, 1, c->image_rows, &c->dist_abs_loc_y);
    blit->bottom = top - rows;
    blit->left = left - 1;
}

static void code_comment(QwJb2Codec *c)
{
    int size = code_num(c, 0, QW_JB2_BIGPOS, &c->dist_comment_length);
    int i;
    if (c->error) return;
    for (i = 0; i < size; ++i)
        (void)code_num(c, 0, 255, &c->dist_comment_byte);
}

static int code_record(QwJb2Codec *c, QwDjvuJb2Image *img)
{
    int rectype = code_record_type(c);
    int need_shape = 0;
    int need_library = 0;
    int need_blit = 0;
    QwDjvuJb2Shape shape;
    QwDjvuJb2Blit blit;
    int match = -1;
    int parent = -1;

    memset(&shape, 0, sizeof(shape));
    memset(&blit, 0, sizeof(blit));

    switch (rectype) {
    case REC_NewMark:
    case REC_NewMarkImageOnly:
    case REC_MatchedRefine:
    case REC_MatchedRefineImageOnly:
    case REC_NonMarkData:
        need_blit = 1;
        need_shape = 1;
        break;
    case REC_NewMarkLibraryOnly:
    case REC_MatchedRefineLibraryOnly:
        need_shape = 1;
        break;
    case REC_MatchedCopy:
        need_blit = 1;
        break;
    default:
        break;
    }

    switch (rectype) {
    case REC_StartOfData: {
        c->image_columns = code_num(c, 0, QW_JB2_BIGPOS, &c->dist_image_size);
        c->image_rows = code_num(c, 0, QW_JB2_BIGPOS, &c->dist_image_size);
        if (c->error || c->image_columns <= 0 || c->image_rows <= 0) {
            c->error = 1;
            break;
        }
        img->width = c->image_columns;
        img->height = c->image_rows;
        c->last_left = 1 + c->image_columns;
        c->last_row_bottom = c->image_rows;
        c->last_row_left = c->last_right = 0;
        fill_shortlist(c, c->last_row_bottom);
        c->refinement_flag = qw_djvu_zp_decode(&c->zp, &c->dist_refinement_flag);
        if (init_library(c, img) != 0) { c->error = 1; break; }
        c->got_start_record = 1;
        break;
    }

    case REC_NewMark:
        need_library = 1;
        if (code_abs_mark_size(c, &shape.bm, 4) != 0) { c->error = 1; break; }
        code_bitmap_direct(c, &shape.bm);
        code_rel_location(c, &blit, shape.bm.height, shape.bm.width);
        break;

    case REC_NewMarkLibraryOnly:
        need_library = 1;
        if (code_abs_mark_size(c, &shape.bm, 4) != 0) { c->error = 1; break; }
        code_bitmap_direct(c, &shape.bm);
        break;

    case REC_NewMarkImageOnly:
        if (code_abs_mark_size(c, &shape.bm, 3) != 0) { c->error = 1; break; }
        code_bitmap_direct(c, &shape.bm);
        code_rel_location(c, &blit, shape.bm.height, shape.bm.width);
        break;

    case REC_MatchedRefine:
    case REC_MatchedRefineLibraryOnly:
    case REC_MatchedRefineImageOnly: {
        const QwDjvuJb2Bitmap *ref;
        int xmin, ymin, xmax, ymax;
        int cw, ch;
        int xd2c, yd2c;
        if (rectype == REC_MatchedRefine) need_library = 1;
        match = code_match_index(c);
        if (match < 0) { c->error = 1; break; }
        parent = c->lib2shape[match];
        shape.parent = parent;
        if (parent < 0 || parent >= img->nshapes) { c->error = 1; break; }
        ref = &img->shapes[parent].bm;
        xmin = c->libinfo[match * 4 + 0];
        ymin = c->libinfo[match * 4 + 1];
        xmax = c->libinfo[match * 4 + 2];
        ymax = c->libinfo[match * 4 + 3];
        cw = (1 + xmax) - xmin;
        ch = (1 + ymax) - ymin;
        if (code_rel_mark_size(c, &shape.bm, cw, ch, 4) != 0) {
            c->error = 1;
            break;
        }
        xd2c = ((1 + (shape.bm.width >> 1)) - shape.bm.width) -
               ((((1 + xmax) - xmin) >> 1) - xmax);
        yd2c = ((1 + (shape.bm.height >> 1)) - shape.bm.height) -
               ((((1 + ymax) - ymin) >> 1) - ymax);
        code_bitmap_cross(c, &shape.bm, ref, xd2c, yd2c);
        if (rectype != REC_MatchedRefineLibraryOnly)
            code_rel_location(c, &blit, shape.bm.height, shape.bm.width);
        break;
    }

    case REC_MatchedCopy: {
        int xmin, ymin, xmax, ymax;
        match = code_match_index(c);
        if (match < 0) { c->error = 1; break; }
        blit.shapeno = c->lib2shape[match];
        xmin = c->libinfo[match * 4 + 0];
        ymin = c->libinfo[match * 4 + 1];
        xmax = c->libinfo[match * 4 + 2];
        ymax = c->libinfo[match * 4 + 3];
        blit.left += xmin;
        blit.bottom += ymin;
        code_rel_location(c, &blit, (1 + ymax) - ymin, (1 + xmax) - xmin);
        blit.left -= xmin;
        blit.bottom -= ymin;
        break;
    }

    case REC_NonMarkData:
        if (code_abs_mark_size(c, &shape.bm, 3) != 0) { c->error = 1; break; }
        code_bitmap_direct(c, &shape.bm);
        code_abs_location(c, &blit, shape.bm.height);
        break;

    case REC_PreservedComment:
        code_comment(c);
        break;

    case REC_RequiredDictOrReset:
        if (!c->got_start_record) {
            int count = code_num(c, 0, QW_JB2_BIGPOS, &c->dist_inherited_count);
            if (c->error || count != 0) c->error = 1;
        } else {
            reset_numcoder(c);
        }
        break;

    case REC_EndOfData:
        break;

    default:
        c->error = 1;
        break;
    }

    if (c->error) {
        if (need_shape) bm_free(&shape.bm);
        return rectype;
    }

    if (need_shape) {
        QwDjvuJb2Shape *stored;
        if (image_add_shape(img, &stored) != 0) {
            c->error = 1;
            bm_free(&shape.bm);
            return rectype;
        }
        stored->parent = shape.parent;
        stored->bm = shape.bm; /* move */
        if (set_shape2lib(c, img->nshapes - 1, -1) != 0) {
            c->error = 1;
            return rectype;
        }
        if (need_library && add_library(c, img->nshapes - 1, stored) < 0) {
            c->error = 1;
            return rectype;
        }
        if (need_blit) {
            blit.shapeno = img->nshapes - 1;
            if (image_add_blit(img, blit) != 0) c->error = 1;
        }
    } else if (rectype == REC_MatchedCopy) {
        if (image_add_blit(img, blit) != 0) c->error = 1;
    }
    return rectype;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

void qw_djvu_jb2_free(struct QwDjvuJb2Image *image)
{
    int i;
    if (!image) return;
    for (i = 0; i < image->nshapes; ++i) bm_free(&image->shapes[i].bm);
    free(image->shapes);
    free(image->blits);
    free(image);
}

int qw_djvu_jb2_decode(const uint8_t *data, size_t size,
                       struct QwDjvuJb2Image **out_image)
{
    QwJb2Codec codec;
    QwDjvuJb2Image *img;
    int rectype = REC_StartOfData;
    int guard = 0;

    if (!data || !out_image) return -1;
    *out_image = NULL;

    memset(&codec, 0, sizeof(codec));
    if (ensure_cells(&codec, 1) != 0) return -1;
    codec.ncells = 1; /* index 0 stays reserved */

    img = (QwDjvuJb2Image *)calloc(1, sizeof(*img));
    if (!img) {
        free(codec.cells);
        free(codec.left);
        free(codec.right);
        return -1;
    }

    qw_djvu_zp_init(&codec.zp, data, size);

    do {
        rectype = code_record(&codec, img);
        if (codec.error) break;
        if (codec.zp.eof) break;
        if (rectype == REC_EndOfData) break;
        if (++guard > 1000000) { codec.error = 1; break; }
    } while (1);

    if (codec.error || !codec.got_start_record) {
        free(codec.cells);
        free(codec.left);
        free(codec.right);
        free(codec.lib2shape);
        free(codec.shape2lib);
        free(codec.libinfo);
        qw_djvu_jb2_free(img);
        return -1;
    }

    free(codec.cells);
    free(codec.left);
    free(codec.right);
    free(codec.lib2shape);
    free(codec.shape2lib);
    free(codec.libinfo);
    *out_image = img;
    return 0;
}

int qw_djvu_jb2_image_size(const struct QwDjvuJb2Image *image, int *width,
                           int *height)
{
    if (!image) return -1;
    if (width) *width = image->width;
    if (height) *height = image->height;
    return 0;
}

int qw_djvu_jb2_render_gray8(const struct QwDjvuJb2Image *image,
                             uint8_t *pixels, size_t stride, size_t height)
{
    int i;

    if (!image || !pixels) return -1;
    if (stride < (size_t)image->width || height < (size_t)image->height)
        return -1;

    for (i = 0; i < (int)height; ++i) memset(pixels + (size_t)i * stride, 255, stride);

    for (i = 0; i < image->nblits; ++i) {
        const QwDjvuJb2Blit *blit = &image->blits[i];
        const QwDjvuJb2Shape *shape;
        int x, y;
        if (blit->shapeno < 0 || blit->shapeno >= image->nshapes) continue;
        shape = &image->shapes[blit->shapeno];
        for (y = 0; y < shape->bm.height; ++y) {
            int py = blit->bottom + y;
            size_t row;
            if (py < 0 || py >= image->height) continue;
            row = (size_t)(image->height - 1 - py);
            for (x = 0; x < shape->bm.width; ++x) {
                int px = blit->left + x;
                if (px < 0 || px >= image->width) continue;
                if (bm_pixel(&shape->bm, x, y))
                    pixels[row * stride + (size_t)px] = 0;
            }
        }
    }
    return 0;
}

/* First-party DjVu document layer. */

#include "djvu_doc.h"

#include "djvu_bzz.h"
#include "djvu_iff.h"
#include "djvu_iw44.h"
#include "djvu_jb2.h"
#include "djvu_parser.h"

#include <stdlib.h>
#include <string.h>

#define QW_DJVU_DOC_MAX_PAGES 10000
#define QW_DJVU_DOC_MAX_INPUT ((size_t)1024u * 1024u * 1024u)
#define QW_DJVU_DOC_MAX_PIXELS ((size_t)100u * 1000u * 1000u)

typedef struct QwDjvuPage {
    const uint8_t *data; /* chunk area of the page FORM */
    size_t size;
} QwDjvuPage;

struct QwDjvuDoc {
    const uint8_t *data;
    size_t size;
    int page_count;
    QwDjvuPage *pages;
};

static uint32_t rd_u16be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static uint32_t rd_u16le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd_u24be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static int add_page(QwDjvuDoc *doc, int *cap, const uint8_t *data, size_t size)
{
    if (doc->page_count >= QW_DJVU_DOC_MAX_PAGES) return -1;
    if (doc->page_count >= *cap) {
        int newcap = *cap ? *cap * 2 : 4;
        QwDjvuPage *grown = (QwDjvuPage *)realloc(
            doc->pages, sizeof(QwDjvuPage) * (size_t)newcap);
        if (!grown) return -1;
        doc->pages = grown;
        *cap = newcap;
    }
    doc->pages[doc->page_count].data = data;
    doc->pages[doc->page_count].size = size;
    ++doc->page_count;
    return 0;
}

int qw_djvu_doc_open(const uint8_t *data, size_t size, QwDjvuDoc **out_doc)
{
    QwDjvuDoc *doc;
    QwDjvuIffForm form;
    size_t page_count = 0;

    if (!out_doc) return -1;
    *out_doc = NULL;
    if (!data || size < 16u || size > QW_DJVU_DOC_MAX_INPUT) return -1;
    if (qw_djvu_probe(data, size) == 0) return -1;
    if (qw_djvu_parse_page_count(data, size, &page_count) != 0) return -1;
    if (qw_djvu_iff_form(data, size, &form) != 0) return -1;

    doc = (QwDjvuDoc *)calloc(1, sizeof(*doc));
    if (!doc) return -1;
    doc->data = data;
    doc->size = size;

    {
        int cap = 0;
        if (strcmp(form.type, "DJVU") == 0) {
            if (add_page(doc, &cap, form.data, form.size) != 0) {
                qw_djvu_doc_free(doc);
                return -1;
            }
        } else {
            size_t cursor = 0;
            QwDjvuIffChunk chunk;
            int rc;
            while ((rc = qw_djvu_iff_next(&form, &cursor, &chunk)) == 1) {
                if (strcmp(chunk.id, "FORM") == 0 && chunk.size >= 4u &&
                    memcmp(chunk.data, "DJVU", 4u) == 0) {
                    if (add_page(doc, &cap, chunk.data + 4, chunk.size - 4u) != 0) {
                        qw_djvu_doc_free(doc);
                        return -1;
                    }
                }
            }
            if (rc < 0) {
                qw_djvu_doc_free(doc);
                return -1;
            }
        }
    }

    if (doc->page_count <= 0) {
        qw_djvu_doc_free(doc);
        return -1;
    }
    *out_doc = doc;
    return 0;
}

void qw_djvu_doc_free(QwDjvuDoc *doc)
{
    if (!doc) return;
    free(doc->pages);
    free(doc);
}

int qw_djvu_doc_page_count(const QwDjvuDoc *doc)
{
    return doc ? doc->page_count : 0;
}

static int page_form(const QwDjvuDoc *doc, int page, QwDjvuIffForm *out)
{
    if (!doc || page < 0 || page >= doc->page_count || !out) return -1;
    out->data = doc->pages[page].data;
    out->size = doc->pages[page].size;
    memcpy(out->type, "DJVU", 4u);
    out->type[4] = '\0';
    return 0;
}

static int find_info(const QwDjvuIffForm *form, const uint8_t **payload,
                     size_t *size)
{
    QwDjvuIffChunk chunk;
    if (qw_djvu_iff_find(form, "INFO", &chunk) != 1) return -1;
    *payload = chunk.data;
    *size = chunk.size;
    return 0;
}

int qw_djvu_doc_page_info(const QwDjvuDoc *doc, int page,
                          QwDjvuDocPageInfo *out_info)
{
    QwDjvuIffForm form;
    const uint8_t *info;
    size_t info_size;

    if (!out_info) return -1;
    memset(out_info, 0, sizeof(*out_info));
    if (page_form(doc, page, &form) != 0) return -1;
    if (find_info(&form, &info, &info_size) != 0) return -1;
    if (info_size < 8u) return -1;

    out_info->width = (int)rd_u16be(info);
    out_info->height = (int)rd_u16be(info + 2);
    out_info->version = info[4];
    out_info->dpi = rd_u16le(info + 6);
    if (out_info->dpi == 0) out_info->dpi = 300;
    out_info->rotation = 0;
    if (info_size >= 10u) {
        switch (info[9] & 0x7u) {
        case 6: out_info->rotation = 90; break;
        case 2: out_info->rotation = 180; break;
        case 5: out_info->rotation = 270; break;
        default: out_info->rotation = 0; break;
        }
    }
    if (out_info->width <= 0 || out_info->height <= 0) return -1;
    return 0;
}

static int render_iw44(const QwDjvuIffForm *form, uint8_t **out_pixels,
                       size_t *out_size, int *out_width, int *out_height)
{
    const uint8_t *chunks[32];
    size_t sizes[32];
    size_t count = 0;
    size_t cursor = 0;
    QwDjvuIffChunk chunk;
    struct QwDjvuIw44Image *image = NULL;
    int w;
    int h;
    uint8_t *rgb;
    int rc;
    int i;

    while (count < 32 && qw_djvu_iff_next(form, &cursor, &chunk) == 1) {
        if (strcmp(chunk.id, "BG44") == 0) {
            chunks[count] = chunk.data;
            sizes[count] = chunk.size;
            ++count;
        }
    }
    if (count == 0) return -1;

    rc = qw_djvu_iw44_decode_chunks(&image, chunks, sizes, count);
    if (rc != 0) return -1;
    if (qw_djvu_iw44_size(image, &w, &h) != 0) {
        qw_djvu_iw44_free(image);
        return -1;
    }
    if ((size_t)w * (size_t)h > QW_DJVU_DOC_MAX_PIXELS) {
        qw_djvu_iw44_free(image);
        return -2;
    }
    rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3u);
    if (!rgb) {
        qw_djvu_iw44_free(image);
        return -1;
    }
    rc = qw_djvu_iw44_render_rgb(image, rgb);
    qw_djvu_iw44_free(image);
    if (rc != 0) {
        free(rgb);
        return -1;
    }
    /* Composite the JB2 mask (if any) as black ink over the background. */
    {
        QwDjvuIffChunk mask;
        if (qw_djvu_iff_find(form, "Sjbz", &mask) == 1) {
            struct QwDjvuJb2Image *jb2 = NULL;
            if (qw_djvu_jb2_decode(mask.data, mask.size, &jb2) == 0) {
                uint8_t *gray = (uint8_t *)malloc((size_t)w * (size_t)h);
                if (gray &&
                    qw_djvu_jb2_render_gray8(jb2, gray, (size_t)w,
                                             (size_t)h) == 0) {
                    for (i = 0; i < w * h; ++i) {
                        if (gray[i] == 0) {
                            rgb[i * 3 + 0] = 0;
                            rgb[i * 3 + 1] = 0;
                            rgb[i * 3 + 2] = 0;
                        }
                    }
                }
                free(gray);
                qw_djvu_jb2_free(jb2);
            }
        }
    }
    *out_pixels = rgb;
    *out_size = (size_t)w * (size_t)h * 3u;
    *out_width = w;
    *out_height = h;
    return 0;
}

static int render_jb2(const QwDjvuIffForm *form, uint8_t **out_pixels,
                      size_t *out_size, int *out_width, int *out_height)
{
    QwDjvuIffChunk chunk;
    struct QwDjvuJb2Image *image = NULL;
    int w;
    int h;
    uint8_t *gray;

    if (qw_djvu_iff_find(form, "Sjbz", &chunk) != 1) return -1;
    if (qw_djvu_jb2_decode(chunk.data, chunk.size, &image) != 0) return -1;
    if (qw_djvu_jb2_image_size(image, &w, &h) != 0) {
        qw_djvu_jb2_free(image);
        return -1;
    }
    if ((size_t)w * (size_t)h > QW_DJVU_DOC_MAX_PIXELS) {
        qw_djvu_jb2_free(image);
        return -2;
    }
    gray = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!gray) {
        qw_djvu_jb2_free(image);
        return -1;
    }
    if (qw_djvu_jb2_render_gray8(image, gray, (size_t)w, (size_t)h) != 0) {
        free(gray);
        qw_djvu_jb2_free(image);
        return -1;
    }
    qw_djvu_jb2_free(image);
    *out_pixels = gray;
    *out_size = (size_t)w * (size_t)h;
    *out_width = w;
    *out_height = h;
    return 0;
}

int qw_djvu_doc_render(const QwDjvuDoc *doc, int page, unsigned subsample,
                       uint8_t **out_pixels, size_t *out_size,
                       int *out_width, int *out_height, int *out_channels)
{
    QwDjvuIffForm form;
    uint8_t *pixels = NULL;
    size_t size = 0;
    int w = 0;
    int h = 0;
    int channels = 0;
    int rc;

    if (!doc || !out_pixels || !out_size || !out_width || !out_height ||
        !out_channels)
        return -1;
    *out_pixels = NULL;
    *out_size = 0;
    if (subsample == 0u) return -1;
    if (page_form(doc, page, &form) != 0) return -1;

    {
        QwDjvuIffChunk probe;
        int has_bg = qw_djvu_iff_find(&form, "BG44", &probe) == 1;
        int has_mask = qw_djvu_iff_find(&form, "Sjbz", &probe) == 1;
        if (has_bg) {
            rc = render_iw44(&form, &pixels, &size, &w, &h);
            channels = 3;
        } else if (has_mask) {
            rc = render_jb2(&form, &pixels, &size, &w, &h);
            channels = 1;
        } else {
            return -1;
        }
        if (rc == -2) return -2;
        if (rc != 0) return -1;
    }

    if (subsample > 1u) {
        int ow = (w + (int)subsample - 1) / (int)subsample;
        int oh = (h + (int)subsample - 1) / (int)subsample;
        size_t osize = (size_t)ow * (size_t)oh * (size_t)channels;
        uint8_t *out = (uint8_t *)malloc(osize ? osize : 1u);
        int x, y, c;
        if (!out) {
            free(pixels);
            return -1;
        }
        for (y = 0; y < oh; ++y) {
            for (x = 0; x < ow; ++x) {
                int sx = x * (int)subsample;
                int sy = y * (int)subsample;
                if (sx >= w) sx = w - 1;
                if (sy >= h) sy = h - 1;
                for (c = 0; c < channels; ++c) {
                    out[((size_t)y * (size_t)ow + (size_t)x) *
                            (size_t)channels + (size_t)c] =
                        pixels[((size_t)sy * (size_t)w + (size_t)sx) *
                                   (size_t)channels + (size_t)c];
                }
            }
        }
        free(pixels);
        pixels = out;
        size = osize;
        w = ow;
        h = oh;
    }

    *out_pixels = pixels;
    *out_size = size;
    *out_width = w;
    *out_height = h;
    *out_channels = channels;
    return 0;
}

int qw_djvu_doc_text(const QwDjvuDoc *doc, int page, char **out_text,
                     size_t *out_len)
{
    QwDjvuIffForm form;
    QwDjvuIffChunk chunk;
    uint8_t *decoded = NULL;
    size_t decoded_size = 0;
    const uint8_t *payload;
    size_t payload_size;
    uint32_t text_len;
    char *text;

    if (out_text) *out_text = NULL;
    if (out_len) *out_len = 0;
    if (!doc || !out_text || !out_len) return -1;
    if (page_form(doc, page, &form) != 0) return -1;

    if (qw_djvu_iff_find(&form, "TXTz", &chunk) == 1) {
        if (qw_djvu_bzz_decode(chunk.data, chunk.size, &decoded,
                               &decoded_size, 1u << 24) != 0)
            return -1;
        payload = decoded;
        payload_size = decoded_size;
    } else if (qw_djvu_iff_find(&form, "TXTa", &chunk) == 1) {
        payload = chunk.data;
        payload_size = chunk.size;
    } else {
        return -1;
    }

    if (payload_size < 3u) {
        free(decoded);
        return -1;
    }
    text_len = rd_u24be(payload);
    if ((size_t)text_len + 3u > payload_size)
        text_len = (uint32_t)(payload_size - 3u);
    text = (char *)malloc((size_t)text_len + 1u);
    if (!text) {
        free(decoded);
        return -1;
    }
    memcpy(text, payload + 3, text_len);
    text[text_len] = '\0';
    free(decoded);
    *out_text = text;
    *out_len = text_len;
    return 0;
}

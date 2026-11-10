/* First-party DjVu document layer: page enumeration, INFO parsing, page
 * rendering (JB2 mask and/or IW44 background) and TXTz text extraction. */

#ifndef QUIREWISE_PAGE_DJVU_DOC_H
#define QUIREWISE_PAGE_DJVU_DOC_H

#include <stddef.h>
#include <stdint.h>

typedef struct QwDjvuDoc QwDjvuDoc;

typedef struct QwDjvuDocPageInfo {
    int width;
    int height;
    unsigned dpi;
    unsigned version;
    unsigned rotation; /* 0, 90, 180, 270 */
} QwDjvuDocPageInfo;

int qw_djvu_doc_open(const uint8_t *data, size_t size, QwDjvuDoc **out_doc);
void qw_djvu_doc_free(QwDjvuDoc *doc);
int qw_djvu_doc_page_count(const QwDjvuDoc *doc);
int qw_djvu_doc_page_info(const QwDjvuDoc *doc, int page,
                          QwDjvuDocPageInfo *out_info);

/* Render a page. *out_pixels is malloc()ed and the caller frees it.
 * channels is 3 (RGB24) when the page has an IW44 background, 1 (GRAY8)
 * for bitonal pages. */
int qw_djvu_doc_render(const QwDjvuDoc *doc, int page, unsigned subsample,
                       uint8_t **out_pixels, size_t *out_size,
                       int *out_width, int *out_height, int *out_channels);

/* Extract the hidden text layer (TXTz/TXTa). Returns 0 and a malloc()ed
 * NUL-terminated UTF-8 string; returns -1 when the page has no text. */
int qw_djvu_doc_text(const QwDjvuDoc *doc, int page, char **out_text,
                     size_t *out_len);

#endif

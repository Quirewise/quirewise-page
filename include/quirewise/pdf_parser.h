#ifndef QUIREWISE_PDF_PARSER_H
#define QUIREWISE_PDF_PARSER_H

#include "quirewise/page_status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QwPdfParser QwPdfParser;

typedef struct QwPdfRect {
    double x0;
    double y0;
    double x1;
    double y1;
} QwPdfRect;

typedef struct QwPdfParserInfo {
    unsigned version_major;
    unsigned version_minor;
    size_t page_count;
    int encrypted;
} QwPdfParserInfo;

typedef struct QwPdfParserPageInfo {
    QwPdfRect media_box;
    QwPdfRect crop_box;
    int rotation;
} QwPdfParserPageInfo;

/*
 * Open a PDF using the native C parser. The input bytes are borrowed and must
 * remain valid until qw_pdf_parser_close(). The reader handles classic
 * cross-reference tables, hybrid-reference xref streams, and compressed
 * objects stored in object streams. FlateDecode streams support PDF TIFF and
 * PNG predictors; other stream filters are reported as unsupported.
 */
QUIREWISE_PAGE_API int qw_pdf_parser_open(const uint8_t *data,
                              size_t size,
                              QwPdfParser **out_parser);
QUIREWISE_PAGE_API int qw_pdf_parser_get_info(const QwPdfParser *parser,
                                   QwPdfParserInfo *out_info);
QUIREWISE_PAGE_API int qw_pdf_parser_get_page_info(const QwPdfParser *parser,
                                        size_t page_index,
                                        QwPdfParserPageInfo *out_info);
QUIREWISE_PAGE_API void qw_pdf_parser_close(QwPdfParser *parser);

#ifdef __cplusplus
}
#endif

#endif

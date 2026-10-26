#ifndef QUIREWISE_CAJ_H
#define QUIREWISE_CAJ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwCajStatus {
    QW_CAJ_OK = 0,
    QW_CAJ_ERR_INVALID_ARGUMENT = -1,
    QW_CAJ_ERR_OUT_OF_MEMORY = -2,
    QW_CAJ_ERR_FORMAT = -3,
    QW_CAJ_ERR_UNSUPPORTED = -4,
    QW_CAJ_ERR_DECODE = -5
} QwCajStatus;

typedef struct QwCajDocument QwCajDocument;
typedef struct QwCajClassicOutlineList QwCajClassicOutlineList;

typedef enum QwCajFigureKind {
    QW_CAJ_FIGURE_PRIVATE_JBIG = 0,
    QW_CAJ_FIGURE_RASTER = 1,
    QW_CAJ_FIGURE_RASTER_BOTTOM_UP = 2,
    QW_CAJ_FIGURE_JBIG2 = 3,
    QW_CAJ_FIGURE_JPEG2000 = 4
} QwCajFigureKind;

typedef struct QwCajPageInfo {
    size_t figure_count;
    size_t encoded_text_size;
    int old_style_text;
    int has_figure_positions;
    int composite_scan_page;
} QwCajPageInfo;

typedef struct QwCajFigureInfo {
    int32_t kind;
    const uint8_t *data;
    size_t size;
    uint16_t x;
    uint16_t y;
} QwCajFigureInfo;

typedef struct QwCajClassicOutlineInfo {
    /* GBK bytes borrowed from the input until the outline list is freed. */
    const uint8_t *title_bytes;
    size_t title_size;
    size_t page_index;
    size_t depth;
} QwCajClassicOutlineInfo;

/* The input buffer is borrowed and must remain valid until document close. */
QwCajStatus qw_caj_document_open(const void *data, size_t size,
                                QwCajDocument **out_document);
size_t qw_caj_document_page_count(const QwCajDocument *document);
QwCajStatus qw_caj_document_page_info(const QwCajDocument *document,
                                      size_t page_index,
                                      QwCajPageInfo *out_info);
QwCajStatus qw_caj_document_figure_info(const QwCajDocument *document,
                                        size_t page_index,
                                        size_t figure_index,
                                        QwCajFigureInfo *out_info);
/* Returns the encoded text record range; it may contain a COMPRESSTEXT block. */
QwCajStatus qw_caj_document_page_text(const QwCajDocument *document,
                                     size_t page_index,
                                     const uint8_t **out_data,
                                     size_t *out_size);
/* Outline titles are borrowed from data, which must remain alive until free. */
QwCajStatus qw_caj_classic_outlines_parse(
    const void *data, size_t size,
    QwCajClassicOutlineList **out_outlines);
size_t qw_caj_classic_outline_count(
    const QwCajClassicOutlineList *outlines);
QwCajStatus qw_caj_classic_outline_get(
    const QwCajClassicOutlineList *outlines, size_t index,
    QwCajClassicOutlineInfo *out_info);
void qw_caj_classic_outlines_free(QwCajClassicOutlineList *outlines);
/* Outputs are owned and released with qw_caj_pdf_data_free. */
QwCajStatus qw_caj_extract_classic_pdf(const void *data, size_t size,
                                       uint8_t **out_pdf,
                                       size_t *out_pdf_size);
QwCajStatus qw_caj_decrypt_kdh_pdf(const void *data, size_t size,
                                   uint8_t **out_pdf,
                                   size_t *out_pdf_size);
void qw_caj_pdf_data_free(uint8_t *pdf_data);
void qw_caj_document_close(QwCajDocument *document);

#ifdef __cplusplus
}
#endif

#endif

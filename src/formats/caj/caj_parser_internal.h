#ifndef QUIREWISE_CAJ_PARSER_INTERNAL_H
#define QUIREWISE_CAJ_PARSER_INTERNAL_H

#include <quirewise/caj.h>

typedef struct QwCajFigure {
    int32_t kind;
    const uint8_t *data;
    size_t size;
    uint16_t x;
    uint16_t y;
} QwCajFigure;

typedef struct QwCajSourcePage {
    QwCajFigure *figures;
    size_t figure_count;
    size_t text_offset;
    size_t encoded_text_size;
    int old_style_text;
    int has_positions;
    int composite;
} QwCajSourcePage;

typedef struct QwCajOutputPage {
    size_t source_page;
    size_t figure_index;
    int composite;
} QwCajOutputPage;

struct QwCajDocument {
    const uint8_t *input;
    size_t input_size;
    QwCajSourcePage *pages;
    size_t page_count;
    QwCajOutputPage *outputs;
    size_t output_count;
};

enum {
    CAJ_C8_PAGE_TABLE = 0x50,
    CAJ_HN_NO_TOC_PAGE_TABLE = 0xd8,
    CAJ_HN_PAGE_COUNT_OFFSET = 0x90,
    CAJ_HN_TOC_COUNT_OFFSET = 0x158,
    CAJ_HN_TOC_START = 0x15c,
    CAJ_TOC_ENTRY_SIZE = 0x134,
    CAJ_CLASSIC_TOC_COUNT_OFFSET = 0x110,
    CAJ_CLASSIC_TOC_START = 0x114,
    CAJ_CLASSIC_TOC_ENTRY_SIZE = 308,
    CAJ_CLASSIC_TOC_TITLE_SIZE = 256,
    CAJ_CLASSIC_TOC_PAGE_OFFSET = 0x118,
    CAJ_CLASSIC_TOC_PAGE_SIZE = 12,
    CAJ_CLASSIC_TOC_LEVEL_OFFSET = 0x130,
    CAJ_PAGE_INFO_SIZE = 20,
    CAJ_MAX_PAGES = 100000,
    CAJ_MAX_OUTLINES = 100000,
    CAJ_MAX_IMAGES_PER_PAGE = 4096,
    CAJ_MAX_PIXELS = 200000000,
    CAJ_MAX_TEXT_BYTES = 64 * 1024 * 1024,
    CAJ_MAX_HTML_BYTES = 256 * 1024 * 1024,
    CAJ_QM_STATE_COUNT = 4096,
    CAJ_QM_TYPICAL_LINE_CX = 0x29c
};

QwCajStatus qw_caj_parse_pages(QwCajDocument *document);
void qw_caj_document_clear(QwCajDocument *document);
QwCajStatus qw_caj_decode_text_section(const uint8_t *section,
                                       size_t section_size,
                                       const uint8_t **out_text,
                                       size_t *out_size,
                                       uint8_t **owned);

#endif

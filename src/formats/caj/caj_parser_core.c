#include "caj_parser_internal.h"

#include "../../core/byte_utils.h"

#include <quirewise/compression.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwCajClassicOutline {
    size_t title_offset;
    size_t title_size;
    size_t page_index;
    size_t depth;
} QwCajClassicOutline;

struct QwCajClassicOutlineList {
    const uint8_t *input;
    QwCajClassicOutline *items;
    size_t count;
};

static int caj_range_ok(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

static size_t caj_find_last_marker(const uint8_t *data, size_t size,
                                   const char *marker, size_t marker_size)
{
    size_t offset;
    if (!data || !marker || !marker_size || marker_size > size) return SIZE_MAX;
    offset = size - marker_size;
    for (;;) {
        if (memcmp(data + offset, marker, marker_size) == 0) return offset;
        if (offset == 0u) break;
        --offset;
    }
    return SIZE_MAX;
}

QwCajStatus qw_caj_decrypt_kdh_pdf(const void *data, size_t size,
                                   uint8_t **out_pdf,
                                   size_t *out_pdf_size)
{
    static const uint8_t key[] = {'F', 'Z', 'H', 'M', 'E', 'I'};
    enum { KDH_HEADER_SIZE = 254 };
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t *pdf;
    size_t pdf_size;
    size_t eof_offset;
    if (!out_pdf || !out_pdf_size) return QW_CAJ_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_pdf_size = 0u;
    if (!bytes || size < 4u || memcmp(bytes, "KDH ", 4u) != 0)
        return QW_CAJ_ERR_INVALID_ARGUMENT;
    if (size <= KDH_HEADER_SIZE) return QW_CAJ_ERR_FORMAT;
    pdf_size = size - KDH_HEADER_SIZE;
    pdf = (uint8_t *)malloc(pdf_size);
    if (!pdf) return QW_CAJ_ERR_OUT_OF_MEMORY;
    for (size_t i = 0u; i < pdf_size; ++i)
        pdf[i] = bytes[KDH_HEADER_SIZE + i] ^ key[i % sizeof(key)];
    if (pdf_size < 5u || memcmp(pdf, "%PDF-", 5u) != 0) {
        free(pdf);
        return QW_CAJ_ERR_FORMAT;
    }
    eof_offset = caj_find_last_marker(pdf, pdf_size, "%%EOF", 5u);
    if (eof_offset != SIZE_MAX) pdf_size = eof_offset + 5u;
    *out_pdf = pdf;
    *out_pdf_size = pdf_size;
    return QW_CAJ_OK;
}

QwCajStatus qw_caj_extract_classic_pdf(const void *data, size_t size,
                                       uint8_t **out_pdf,
                                       size_t *out_pdf_size)
{
    static const uint8_t pdf_header[] = "%PDF-1.3\r\n";
    static const uint8_t line_break[] = "\r\n";
    static const char end_object[] = "endobj";
    const uint8_t *bytes = (const uint8_t *)data;
    size_t start = SIZE_MAX;
    size_t payload_end = size;
    if (!out_pdf || !out_pdf_size) return QW_CAJ_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_pdf_size = 0u;
    if (!bytes || size < 3u || memcmp(bytes, "CAJ", 3u) != 0)
        return QW_CAJ_ERR_INVALID_ARGUMENT;

    /* CAJ stores a pointer to a second pointer before the embedded PDF. */
    if (caj_range_ok(size, 0x14u, 4u)) {
        int32_t pointer = qw_read_i32_le(bytes + 0x14u);
        if (pointer >= 0 && caj_range_ok(size, (size_t)pointer, 4u)) {
            int32_t pdf_offset = qw_read_i32_le(bytes + (size_t)pointer);
            if (pdf_offset >= 0 && (size_t)pdf_offset < size) {
                size_t marker = caj_find_last_marker(
                    bytes + (size_t)pdf_offset, size - (size_t)pdf_offset,
                    end_object, sizeof(end_object) - 1u);
                if (marker != SIZE_MAX) {
                    start = (size_t)pdf_offset;
                    payload_end = start + marker + sizeof(end_object) - 1u;
                } else if (caj_range_ok(size, (size_t)pdf_offset, 5u) &&
                           memcmp(bytes + (size_t)pdf_offset, "%PDF-", 5u) == 0) {
                    start = (size_t)pdf_offset;
                }
            }
        }
    }

    if (start != SIZE_MAX && payload_end > start) {
        size_t payload_size = payload_end - start;
        int has_pdf_header = payload_size >= 5u &&
                             memcmp(bytes + start, "%PDF-", 5u) == 0;
        size_t prefix_size = has_pdf_header ? 0u : sizeof(pdf_header) - 1u;
        size_t output_size;
        uint8_t *output;
        if (payload_size > SIZE_MAX - prefix_size - sizeof(line_break) + 1u)
            return QW_CAJ_ERR_OUT_OF_MEMORY;
        output_size = prefix_size + payload_size + sizeof(line_break) - 1u;
        output = (uint8_t *)malloc(output_size ? output_size : 1u);
        if (!output) return QW_CAJ_ERR_OUT_OF_MEMORY;
        if (prefix_size) memcpy(output, pdf_header, prefix_size);
        memcpy(output + prefix_size, bytes + start, payload_size);
        memcpy(output + prefix_size + payload_size,
               line_break, sizeof(line_break) - 1u);
        *out_pdf = output;
        *out_pdf_size = output_size;
        return QW_CAJ_OK;
    }

    /* Keep compatibility with CAJ variants containing a direct PDF marker. */
    if (size >= 5u) {
        for (size_t i = 0u; i <= size - 5u; ++i) {
            if (memcmp(bytes + i, "%PDF-", 5u) == 0) {
                size_t output_size = size - i;
                uint8_t *output = (uint8_t *)malloc(output_size);
                if (!output) return QW_CAJ_ERR_OUT_OF_MEMORY;
                memcpy(output, bytes + i, output_size);
                *out_pdf = output;
                *out_pdf_size = output_size;
                return QW_CAJ_OK;
            }
        }
    }
    return QW_CAJ_ERR_UNSUPPORTED;
}

void qw_caj_pdf_data_free(uint8_t *pdf_data)
{
    free(pdf_data);
}

static int caj_append_position(uint16_t **positions, size_t *count,
                               size_t *capacity, uint16_t x, uint16_t y)
{
    uint16_t *grown;
    size_t next;
    if (*count >= CAJ_MAX_IMAGES_PER_PAGE + 1u) return 0;
    if (*count == *capacity) {
        next = *capacity ? *capacity * 2u : 8u;
        if (next > CAJ_MAX_IMAGES_PER_PAGE + 1u)
            next = CAJ_MAX_IMAGES_PER_PAGE + 1u;
        if (next < *capacity || next > SIZE_MAX / (2u * sizeof(uint16_t)))
            return 0;
        grown = (uint16_t *)realloc(*positions,
                                    next * 2u * sizeof(uint16_t));
        if (!grown) return 0;
        *positions = grown;
        *capacity = next;
    }
    (*positions)[*count * 2u] = x;
    (*positions)[*count * 2u + 1u] = y;
    ++*count;
    return 1;
}

static int caj_parse_positions(const uint8_t *text, size_t size,
                               int old_style, uint16_t **positions,
                               size_t *count)
{
    size_t offset = 0u;
    size_t capacity = 0u;
    *positions = NULL;
    *count = 0u;
    while (offset <= size && size - offset >= 2u) {
        uint16_t marker = qw_read_u16_le(text + offset);
        offset += 2u;
        if (marker == 0x800au) {
            if (!caj_range_ok(size, offset, 26u) ||
                !caj_append_position(positions, count, &capacity,
                                     qw_read_u16_le(text + offset + 2u),
                                     qw_read_u16_le(text + offset + 4u)))
                return 0;
            offset += 26u;
        } else if (marker == 0x8004u) {
            break;
        } else if ((marker == 0x8001u || marker == 0x8070u) && old_style) {
            if (!caj_range_ok(size, offset, 2u)) return 0;
            offset += 2u;
            while (offset < size) {
                if (size - offset < 2u || text[offset + 1u] == 0x80u) break;
                if (offset > SIZE_MAX - 4u) return 0;
                offset += 4u;
            }
        } else if (marker == 0x8001u) {
            if (caj_range_ok(size, offset, 4u) &&
                qw_read_u16_le(text + offset + 2u) == 0x8002u) {
                size_t cell;
                if (offset > SIZE_MAX - 6u) return 0;
                cell = offset + 6u;
                while (cell <= size && size - cell >= 8u) {
                    uint16_t cell_marker = qw_read_u16_le(text + cell);
                    if (cell_marker == 0x8001u || cell_marker == 0x8002u ||
                        cell_marker == 0x800au || cell_marker == 0x8004u)
                        break;
                    if (cell > SIZE_MAX - 4u) return 0;
                    cell += 4u;
                }
                offset = cell;
            } else {
                if (offset > SIZE_MAX - 6u) return 0;
                offset += 6u;
            }
        } else {
            offset += 2u;
        }
        if (offset > size) return 0;
    }
    return 1;
}

QwCajStatus qw_caj_decode_text_section(const uint8_t *section,
                                       size_t section_size,
                                       const uint8_t **out_text,
                                       size_t *out_size,
                                       uint8_t **owned)
{
    size_t marker = SIZE_MAX;
    uint32_t expanded;
    uint8_t *buffer;
    if (!section || !out_text || !out_size || !owned)
        return QW_CAJ_ERR_INVALID_ARGUMENT;
    *owned = NULL;
    if (section_size >= 12u && memcmp(section, "COMPRESSTEXT", 12u) == 0)
        marker = 0u;
    else if (section_size >= 20u &&
             memcmp(section + 8u, "COMPRESSTEXT", 12u) == 0)
        marker = 8u;
    if (marker == SIZE_MAX) {
        *out_text = section;
        *out_size = section_size;
        return QW_CAJ_OK;
    }
    if (!caj_range_ok(section_size, marker + 12u, 4u))
        return QW_CAJ_ERR_FORMAT;
    expanded = qw_read_u32_le(section + marker + 12u);
    if (expanded > 256u * 1024u * 1024u) return QW_CAJ_ERR_UNSUPPORTED;
    if (!caj_range_ok(section_size, marker + 16u,
                      section_size - (marker + 16u)))
        return QW_CAJ_ERR_FORMAT;
    buffer = (uint8_t *)malloc(expanded ? (size_t)expanded : 1u);
    if (!buffer) return QW_CAJ_ERR_OUT_OF_MEMORY;
    if (!qw_compression_inflate_zlib(section + marker + 16u,
                                     section_size - marker - 16u,
                                     buffer, (size_t)expanded)) {
        free(buffer);
        return QW_CAJ_ERR_DECODE;
    }
    *owned = buffer;
    *out_text = buffer;
    *out_size = expanded;
    return QW_CAJ_OK;
}

void qw_caj_document_clear(QwCajDocument *document)
{
    if (!document) return;
    if (document->pages) {
        for (size_t i = 0u; i < document->page_count; ++i)
            free(document->pages[i].figures);
    }
    free(document->pages);
    free(document->outputs);
    document->pages = NULL;
    document->outputs = NULL;
    document->page_count = 0u;
    document->output_count = 0u;
}

QwCajStatus qw_caj_parse_pages(QwCajDocument *document)
{
    const uint8_t *bytes;
    size_t size;
    size_t page_count_offset;
    size_t page_table_offset;
    int32_t signed_count;
    if (!document || !document->input || !document->input_size)
        return QW_CAJ_ERR_INVALID_ARGUMENT;
    bytes = document->input;
    size = document->input_size;
    if (size < 4u) return QW_CAJ_ERR_FORMAT;
    if (bytes[0] == 0xc8u) {
        page_count_offset = 8u;
        page_table_offset = CAJ_C8_PAGE_TABLE;
    } else if (memcmp(bytes, "HN\xc8\x00", 4u) == 0) {
        page_count_offset = CAJ_HN_PAGE_COUNT_OFFSET;
        page_table_offset = CAJ_HN_NO_TOC_PAGE_TABLE;
    } else if (memcmp(bytes, "HN\x00\x00", 4u) == 0) {
        int32_t toc_count;
        size_t table_bytes;
        if (!caj_range_ok(size, CAJ_HN_TOC_COUNT_OFFSET, 4u))
            return QW_CAJ_ERR_FORMAT;
        toc_count = qw_read_i32_le(bytes + CAJ_HN_TOC_COUNT_OFFSET);
        if (toc_count < 0 || (size_t)toc_count > CAJ_MAX_PAGES ||
            (size_t)toc_count > (SIZE_MAX - CAJ_HN_TOC_START) /
                                     CAJ_TOC_ENTRY_SIZE)
            return QW_CAJ_ERR_FORMAT;
        table_bytes = (size_t)toc_count * CAJ_TOC_ENTRY_SIZE;
        page_count_offset = CAJ_HN_PAGE_COUNT_OFFSET;
        page_table_offset = CAJ_HN_TOC_START + table_bytes;
    } else if (memcmp(bytes, "CAJ", 3u) == 0) {
        return QW_CAJ_ERR_UNSUPPORTED;
    } else {
        return QW_CAJ_ERR_UNSUPPORTED;
    }
    if (!caj_range_ok(size, page_count_offset, 4u))
        return QW_CAJ_ERR_FORMAT;
    signed_count = qw_read_i32_le(bytes + page_count_offset);
    if (signed_count <= 0 || (size_t)signed_count > CAJ_MAX_PAGES)
        return QW_CAJ_ERR_FORMAT;
    document->page_count = (size_t)signed_count;
    if (document->page_count > (SIZE_MAX - page_table_offset) /
                                   CAJ_PAGE_INFO_SIZE ||
        !caj_range_ok(size, page_table_offset,
                      document->page_count * CAJ_PAGE_INFO_SIZE))
        return QW_CAJ_ERR_FORMAT;
    document->pages = (QwCajSourcePage *)calloc(document->page_count,
                                                 sizeof(*document->pages));
    if (!document->pages) return QW_CAJ_ERR_OUT_OF_MEMORY;

    for (size_t page_index = 0u; page_index < document->page_count;
         ++page_index) {
        size_t info_offset = page_table_offset + page_index * CAJ_PAGE_INFO_SIZE;
        int32_t data_offset = qw_read_i32_le(bytes + info_offset);
        int32_t text_size = qw_read_i32_le(bytes + info_offset + 4u);
        int16_t image_count = (int16_t)qw_read_u16_le(bytes + info_offset + 8u);
        int32_t next_page_offset = qw_read_i32_le(bytes + info_offset + 16u);
        size_t text_end;
        uint8_t *owned_text = NULL;
        const uint8_t *text = NULL;
        size_t decoded_text_size = 0u;
        uint16_t *positions = NULL;
        size_t position_count = 0u;
        QwCajStatus status;
        if (data_offset < 0 || text_size < 0 ||
            image_count > CAJ_MAX_IMAGES_PER_PAGE)
            return QW_CAJ_ERR_FORMAT;
        if ((size_t)data_offset > SIZE_MAX - (size_t)text_size)
            return QW_CAJ_ERR_FORMAT;
        text_end = (size_t)data_offset + (size_t)text_size;
        if (!caj_range_ok(size, (size_t)data_offset, (size_t)text_size))
            return QW_CAJ_ERR_FORMAT;
        status = qw_caj_decode_text_section(bytes + data_offset,
                                            (size_t)text_size, &text,
                                            &decoded_text_size, &owned_text);
        if (status != QW_CAJ_OK) return status;
        document->pages[page_index].text_offset = (size_t)data_offset;
        document->pages[page_index].encoded_text_size = (size_t)text_size;
        document->pages[page_index].old_style_text =
            next_page_offset > data_offset;
        if (!caj_parse_positions(text, decoded_text_size,
                                 next_page_offset > data_offset,
                                 &positions, &position_count)) {
            free(positions);
            free(owned_text);
            return QW_CAJ_ERR_FORMAT;
        }
        free(owned_text);
        if (image_count < 0) image_count = 0;
        if (image_count > 0) {
            document->pages[page_index].figures =
                (QwCajFigure *)calloc((size_t)image_count,
                                      sizeof(QwCajFigure));
            if (!document->pages[page_index].figures) {
                free(positions);
                return QW_CAJ_ERR_OUT_OF_MEMORY;
            }
        }
        document->pages[page_index].figure_count = (size_t)image_count;
        document->pages[page_index].has_positions =
            position_count == (size_t)image_count;
        for (size_t figure_index = 0u; figure_index < (size_t)image_count;
             ++figure_index) {
            size_t figure_header;
            int32_t kind;
            int32_t payload_offset;
            int32_t payload_size;
            const QwCajFigure *previous;
            if (!figure_index) {
                figure_header = text_end;
            } else {
                previous = &document->pages[page_index]
                               .figures[figure_index - 1u];
                figure_header = (size_t)(previous->data - bytes) +
                                previous->size;
            }
            if (!caj_range_ok(size, figure_header, 12u)) {
                free(positions);
                return QW_CAJ_ERR_FORMAT;
            }
            kind = qw_read_i32_le(bytes + figure_header);
            payload_offset = qw_read_i32_le(bytes + figure_header + 4u);
            payload_size = qw_read_i32_le(bytes + figure_header + 8u);
            if (payload_offset < 0 || payload_size < 0 ||
                (size_t)payload_offset != figure_header + 12u ||
                (size_t)payload_offset > SIZE_MAX - (size_t)payload_size ||
                !caj_range_ok(size, (size_t)payload_offset,
                              (size_t)payload_size)) {
                free(positions);
                return QW_CAJ_ERR_FORMAT;
            }
            document->pages[page_index].figures[figure_index].kind = kind;
            document->pages[page_index].figures[figure_index].data =
                bytes + payload_offset;
            document->pages[page_index].figures[figure_index].size =
                (size_t)payload_size;
            if (position_count == (size_t)image_count) {
                document->pages[page_index].figures[figure_index].x =
                    positions[figure_index * 2u];
                document->pages[page_index].figures[figure_index].y =
                    positions[figure_index * 2u + 1u];
            }
        }
        free(positions);
        document->pages[page_index].composite = image_count > 1 &&
            document->pages[page_index].has_positions &&
            document->pages[page_index].figures[0].x == 0u &&
            document->pages[page_index].figures[0].y == 0u;
    }
    return QW_CAJ_OK;
}

QwCajStatus qw_caj_document_open(const void *data, size_t size,
                                QwCajDocument **out_document)
{
    QwCajDocument *document;
    QwCajStatus status;
    if (!out_document) return QW_CAJ_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (!data || !size) return QW_CAJ_ERR_INVALID_ARGUMENT;
    document = (QwCajDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_CAJ_ERR_OUT_OF_MEMORY;
    document->input = (const uint8_t *)data;
    document->input_size = size;
    status = qw_caj_parse_pages(document);
    if (status != QW_CAJ_OK) {
        qw_caj_document_clear(document);
        free(document);
        return status;
    }
    *out_document = document;
    return QW_CAJ_OK;
}

size_t qw_caj_document_page_count(const QwCajDocument *document)
{
    return document ? document->page_count : 0u;
}

QwCajStatus qw_caj_document_page_info(const QwCajDocument *document,
                                      size_t page_index,
                                      QwCajPageInfo *out_info)
{
    const QwCajSourcePage *page;
    if (!document || !out_info || page_index >= document->page_count)
        return QW_CAJ_ERR_INVALID_ARGUMENT;
    page = &document->pages[page_index];
    out_info->figure_count = page->figure_count;
    out_info->encoded_text_size = page->encoded_text_size;
    out_info->old_style_text = page->old_style_text;
    out_info->has_figure_positions = page->has_positions;
    out_info->composite_scan_page = page->composite;
    return QW_CAJ_OK;
}

QwCajStatus qw_caj_document_figure_info(const QwCajDocument *document,
                                        size_t page_index,
                                        size_t figure_index,
                                        QwCajFigureInfo *out_info)
{
    const QwCajFigure *figure;
    if (!document || !out_info || page_index >= document->page_count ||
        figure_index >= document->pages[page_index].figure_count)
        return QW_CAJ_ERR_INVALID_ARGUMENT;
    figure = &document->pages[page_index].figures[figure_index];
    out_info->kind = figure->kind;
    out_info->data = figure->data;
    out_info->size = figure->size;
    out_info->x = figure->x;
    out_info->y = figure->y;
    return QW_CAJ_OK;
}

QwCajStatus qw_caj_document_page_text(const QwCajDocument *document,
                                     size_t page_index,
                                     const uint8_t **out_data,
                                     size_t *out_size)
{
    const QwCajSourcePage *page;
    if (!document || !out_data || !out_size ||
        page_index >= document->page_count)
        return QW_CAJ_ERR_INVALID_ARGUMENT;
    page = &document->pages[page_index];
    *out_data = document->input + page->text_offset;
    *out_size = page->encoded_text_size;
    return QW_CAJ_OK;
}

void qw_caj_document_close(QwCajDocument *document)
{
    if (!document) return;
    qw_caj_document_clear(document);
    free(document);
}

static int caj_parse_ascii_page_number(const uint8_t *field,
                                      size_t field_size,
                                      size_t page_count,
                                      size_t *out_page)
{
    size_t value = 0u;
    int saw_digit = 0;
    int ended = 0;
    for (size_t i = 0u; i < field_size; ++i) {
        uint8_t ch = field[i];
        if (ch >= '0' && ch <= '9') {
            if (ended || value > (SIZE_MAX - (size_t)(ch - '0')) / 10u)
                return 0;
            value = value * 10u + (size_t)(ch - '0');
            saw_digit = 1;
        } else if (ch == 0u) {
            ended = 1;
        } else if (ch == ' ' || ch == '\t') {
            if (saw_digit) ended = 1;
        } else {
            return 0;
        }
    }
    if (!saw_digit || value == 0u || value > page_count) return 0;
    *out_page = value - 1u;
    return 1;
}

QwCajStatus qw_caj_classic_outlines_parse(
    const void *data, size_t size,
    QwCajClassicOutlineList **out_outlines)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwCajClassicOutlineList *outlines;
    uint32_t raw_page_count;
    uint32_t raw_toc_count;
    size_t page_count;
    size_t toc_count;
    size_t valid_count = 0u;
    size_t previous_depth = 0u;
    if (!out_outlines) return QW_CAJ_ERR_INVALID_ARGUMENT;
    *out_outlines = NULL;
    if (!bytes || !size) return QW_CAJ_ERR_INVALID_ARGUMENT;
    if (size < 3u || memcmp(bytes, "CAJ", 3u) != 0)
        return QW_CAJ_ERR_UNSUPPORTED;
    if (!caj_range_ok(size, 0x10u, 4u) ||
        !caj_range_ok(size, CAJ_CLASSIC_TOC_COUNT_OFFSET, 4u))
        return QW_CAJ_ERR_FORMAT;
    raw_page_count = qw_read_u32_le(bytes + 0x10u);
    raw_toc_count = qw_read_u32_le(bytes + CAJ_CLASSIC_TOC_COUNT_OFFSET);
    page_count = (size_t)raw_page_count;
    toc_count = (size_t)raw_toc_count;
    if (!page_count || page_count > CAJ_MAX_PAGES || !toc_count ||
        toc_count > CAJ_MAX_OUTLINES ||
        toc_count > (SIZE_MAX - CAJ_CLASSIC_TOC_START) /
                        CAJ_CLASSIC_TOC_ENTRY_SIZE ||
        !caj_range_ok(size, CAJ_CLASSIC_TOC_START,
                      toc_count * CAJ_CLASSIC_TOC_ENTRY_SIZE) ||
        toc_count > SIZE_MAX / sizeof(QwCajClassicOutline))
        return QW_CAJ_ERR_FORMAT;
    outlines = (QwCajClassicOutlineList *)calloc(1u, sizeof(*outlines));
    if (!outlines) return QW_CAJ_ERR_OUT_OF_MEMORY;
    outlines->items = (QwCajClassicOutline *)calloc(
        toc_count, sizeof(*outlines->items));
    if (!outlines->items) {
        free(outlines);
        return QW_CAJ_ERR_OUT_OF_MEMORY;
    }
    outlines->input = bytes;
    for (size_t i = 0u; i < toc_count; ++i) {
        const uint8_t *entry = bytes + CAJ_CLASSIC_TOC_START +
                               i * CAJ_CLASSIC_TOC_ENTRY_SIZE;
        size_t title_size = 0u;
        size_t page_index;
        int32_t level;
        size_t depth;
        while (title_size < CAJ_CLASSIC_TOC_TITLE_SIZE && entry[title_size])
            ++title_size;
        while (title_size &&
               (entry[title_size - 1u] == ' ' ||
                entry[title_size - 1u] == '\t' ||
                entry[title_size - 1u] == '\r' ||
                entry[title_size - 1u] == '\n')) {
            --title_size;
        }
        level = qw_read_i32_le(entry + CAJ_CLASSIC_TOC_LEVEL_OFFSET);
        if (!title_size || level <= 0 ||
            !caj_parse_ascii_page_number(entry + CAJ_CLASSIC_TOC_PAGE_OFFSET,
                                         CAJ_CLASSIC_TOC_PAGE_SIZE,
                                         page_count, &page_index))
            continue;
        depth = (size_t)(level - 1);
        if (!valid_count) depth = 0u;
        else if (depth > previous_depth + 1u) depth = previous_depth + 1u;
        outlines->items[valid_count].title_offset =
            (size_t)(entry - bytes);
        outlines->items[valid_count].title_size = title_size;
        outlines->items[valid_count].page_index = page_index;
        outlines->items[valid_count].depth = depth;
        previous_depth = depth;
        ++valid_count;
    }
    if (!valid_count) {
        qw_caj_classic_outlines_free(outlines);
        return QW_CAJ_ERR_UNSUPPORTED;
    }
    outlines->count = valid_count;
    *out_outlines = outlines;
    return QW_CAJ_OK;
}

size_t qw_caj_classic_outline_count(
    const QwCajClassicOutlineList *outlines)
{
    return outlines ? outlines->count : 0u;
}

QwCajStatus qw_caj_classic_outline_get(
    const QwCajClassicOutlineList *outlines, size_t index,
    QwCajClassicOutlineInfo *out_info)
{
    const QwCajClassicOutline *outline;
    if (!outlines || !out_info || index >= outlines->count)
        return QW_CAJ_ERR_INVALID_ARGUMENT;
    outline = &outlines->items[index];
    out_info->title_bytes = outlines->input + outline->title_offset;
    out_info->title_size = outline->title_size;
    out_info->page_index = outline->page_index;
    out_info->depth = outline->depth;
    return QW_CAJ_OK;
}

void qw_caj_classic_outlines_free(QwCajClassicOutlineList *outlines)
{
    if (!outlines) return;
    free(outlines->items);
    free(outlines);
}

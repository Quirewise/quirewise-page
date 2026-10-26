#include "quirewise/pdf_parser.h"
#include "pdf_stream_codec.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    PDF_PARSER_MAX_XREF_SECTIONS = 64,
    PDF_PARSER_MAX_XREF_ENTRIES = 5000000,
    PDF_PARSER_MAX_OBJECT_NUMBER = 10000000,
    PDF_PARSER_MAX_VALUE_DEPTH = 64,
    PDF_PARSER_MAX_PAGE_DEPTH = 128,
    PDF_PARSER_MAX_PAGES = 1000000,
    PDF_PARSER_MAX_OBJECT_STREAMS = 100000
};

#define PDF_PARSER_MAX_DECODED_STREAM ((size_t)256u * 1024u * 1024u)
#define PDF_PARSER_MAX_TOTAL_DECODED_STREAMS ((size_t)512u * 1024u * 1024u)

typedef struct PdfXrefEntry {
    uint32_t object_number;
    uint32_t generation;
    size_t offset;
    uint32_t object_stream_number;
    uint32_t object_stream_index;
    unsigned revision;
    unsigned type;
    unsigned priority;
    int in_use;
} PdfXrefEntry;

typedef struct PdfSpan {
    const uint8_t *data;
    size_t data_size;
    size_t start;
    size_t end;
} PdfSpan;

typedef enum PdfValueKind {
    PDF_VALUE_OTHER = 0,
    PDF_VALUE_NUMBER,
    PDF_VALUE_REFERENCE,
    PDF_VALUE_NAME,
    PDF_VALUE_ARRAY,
    PDF_VALUE_DICTIONARY
} PdfValueKind;

typedef struct PdfValue {
    PdfValueKind kind;
    PdfSpan span;
    double number;
    uint32_t object_number;
    uint32_t generation;
} PdfValue;

typedef struct PdfObjectStream {
    uint32_t object_number;
    uint32_t *object_numbers;
    size_t *object_offsets;
    size_t object_count;
    size_t first_object_offset;
    uint8_t *decoded_data;
    size_t decoded_size;
} PdfObjectStream;

struct QwPdfParser {
    const uint8_t *data;
    size_t size;
    PdfXrefEntry *xref;
    size_t xref_count;
    size_t xref_capacity;
    PdfObjectStream *object_streams;
    size_t object_stream_count;
    size_t object_stream_capacity;
    size_t total_decoded_stream_bytes;
    uint32_t root_object;
    uint32_t root_generation;
    int has_root;
    QwPdfParserInfo info;
    QwPdfParserPageInfo *pages;
    size_t page_count;
    size_t page_capacity;
};

static int pdf_object_value(QwPdfParser *parser,
                            uint32_t object_number,
                            uint32_t generation,
                            PdfValue *out_value);
static int pdf_resolve_value(QwPdfParser *parser,
                             PdfValue value,
                             PdfValue *out_value);
static PdfObjectStream *pdf_object_stream_load(QwPdfParser *parser,
                                               uint32_t object_number);

typedef struct PdfParserCursor {
    const uint8_t *data;
    size_t size;
    size_t position;
} PdfParserCursor;

typedef struct PdfInheritedPageInfo {
    QwPdfRect media_box;
    QwPdfRect crop_box;
    int rotation;
    int has_media_box;
    int has_crop_box;
    int has_rotation;
} PdfInheritedPageInfo;

typedef struct PdfPageWalk {
    QwPdfParser *parser;
    uint32_t stack[PDF_PARSER_MAX_PAGE_DEPTH];
    size_t depth;
} PdfPageWalk;

static int pdf_is_white(uint8_t c)
{
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

static int pdf_is_delimiter(uint8_t c)
{
    return pdf_is_white(c) || c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

static void pdf_skip_space(PdfParserCursor *cursor)
{
    while (cursor->position < cursor->size) {
        uint8_t c = cursor->data[cursor->position];
        if (pdf_is_white(c)) {
            ++cursor->position;
        } else if (c == '%') {
            while (cursor->position < cursor->size &&
                   cursor->data[cursor->position] != '\n' &&
                   cursor->data[cursor->position] != '\r') {
                ++cursor->position;
            }
        } else {
            break;
        }
    }
}

static int pdf_parse_unsigned(PdfParserCursor *cursor, uint64_t *out_value)
{
    uint64_t value = 0;
    size_t start;
    pdf_skip_space(cursor);
    start = cursor->position;
    while (cursor->position < cursor->size) {
        uint8_t c = cursor->data[cursor->position];
        unsigned digit;
        if (c < '0' || c > '9') {
            break;
        }
        digit = (unsigned)(c - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return 0;
        }
        value = value * 10u + digit;
        ++cursor->position;
    }
    if (cursor->position == start) {
        return 0;
    }
    *out_value = value;
    return 1;
}

static int pdf_parse_number(PdfParserCursor *cursor, double *out_value,
                            int *out_integer, uint64_t *out_unsigned)
{
    size_t start;
    size_t end;
    size_t i;
    double value = 0.0;
    int negative = 0;
    int saw_digit = 0;
    int fractional = 0;
    double scale = 0.1;

    pdf_skip_space(cursor);
    start = cursor->position;
    if (start < cursor->size &&
        (cursor->data[start] == '+' || cursor->data[start] == '-')) {
        negative = cursor->data[start] == '-';
        ++cursor->position;
    }
    while (cursor->position < cursor->size) {
        uint8_t c = cursor->data[cursor->position];
        if (c >= '0' && c <= '9') {
            saw_digit = 1;
            if (!fractional) {
                value = value * 10.0 + (double)(c - '0');
            } else {
                value += (double)(c - '0') * scale;
                scale *= 0.1;
            }
            ++cursor->position;
        } else if (c == '.' && !fractional) {
            fractional = 1;
            ++cursor->position;
        } else {
            break;
        }
    }
    end = cursor->position;
    if (!saw_digit || (end < cursor->size && !pdf_is_delimiter(cursor->data[end]))) {
        cursor->position = start;
        return 0;
    }
    if (negative) {
        value = -value;
    }
    if (!isfinite(value)) {
        cursor->position = start;
        return 0;
    }
    *out_value = value;
    *out_integer = !fractional;
    *out_unsigned = 0;
    if (!negative && !fractional) {
        uint64_t integer = 0;
        for (i = start; i < end; ++i) {
            uint8_t c = cursor->data[i];
            if (c < '0' || c > '9') {
                continue;
            }
            if (integer > (UINT64_MAX - (uint64_t)(c - '0')) / 10u) {
                return 0;
            }
            integer = integer * 10u + (uint64_t)(c - '0');
        }
        *out_unsigned = integer;
    }
    return 1;
}

static int pdf_skip_literal_string(PdfParserCursor *cursor)
{
    size_t depth = 0;
    if (cursor->position >= cursor->size ||
        cursor->data[cursor->position] != '(') {
        return 0;
    }
    ++cursor->position;
    depth = 1;
    while (cursor->position < cursor->size && depth) {
        uint8_t c = cursor->data[cursor->position++];
        if (c == '\\') {
            if (cursor->position < cursor->size) {
                if (cursor->data[cursor->position] == '\r') {
                    ++cursor->position;
                    if (cursor->position < cursor->size &&
                        cursor->data[cursor->position] == '\n') {
                        ++cursor->position;
                    }
                } else {
                    ++cursor->position;
                }
            }
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        }
    }
    return depth == 0;
}

static int pdf_parse_name(PdfParserCursor *cursor, PdfSpan *out_span)
{
    size_t start;
    if (cursor->position >= cursor->size ||
        cursor->data[cursor->position] != '/') {
        return 0;
    }
    start = cursor->position++;
    while (cursor->position < cursor->size &&
           !pdf_is_delimiter(cursor->data[cursor->position])) {
        if (cursor->data[cursor->position] == '#' &&
            cursor->position + 2u < cursor->size) {
            cursor->position += 3u;
        } else {
            ++cursor->position;
        }
    }
    out_span->data = cursor->data;
    out_span->data_size = cursor->size;
    out_span->start = start;
    out_span->end = cursor->position;
    return 1;
}

static int pdf_parse_value(PdfParserCursor *cursor, unsigned depth,
                           PdfValue *out_value);

static int pdf_parse_array(PdfParserCursor *cursor, unsigned depth)
{
    if (depth > PDF_PARSER_MAX_VALUE_DEPTH ||
        cursor->position >= cursor->size || cursor->data[cursor->position] != '[') {
        return 0;
    }
    ++cursor->position;
    for (;;) {
        PdfValue item;
        pdf_skip_space(cursor);
        if (cursor->position >= cursor->size) {
            return 0;
        }
        if (cursor->data[cursor->position] == ']') {
            ++cursor->position;
            return 1;
        }
        if (!pdf_parse_value(cursor, depth + 1u, &item)) {
            return 0;
        }
    }
}

static int pdf_parse_dictionary(PdfParserCursor *cursor, unsigned depth)
{
    if (depth > PDF_PARSER_MAX_VALUE_DEPTH ||
        cursor->position + 1u >= cursor->size ||
        cursor->data[cursor->position] != '<' ||
        cursor->data[cursor->position + 1u] != '<') {
        return 0;
    }
    cursor->position += 2u;
    for (;;) {
        PdfSpan name;
        PdfValue value;
        pdf_skip_space(cursor);
        if (cursor->position + 1u >= cursor->size) {
            return 0;
        }
        if (cursor->data[cursor->position] == '>' &&
            cursor->data[cursor->position + 1u] == '>') {
            cursor->position += 2u;
            return 1;
        }
        if (!pdf_parse_name(cursor, &name) ||
            !pdf_parse_value(cursor, depth + 1u, &value)) {
            return 0;
        }
    }
}

static int pdf_parse_value(PdfParserCursor *cursor, unsigned depth,
                           PdfValue *out_value)
{
    size_t start;
    uint8_t c;
    if (depth > PDF_PARSER_MAX_VALUE_DEPTH) {
        return 0;
    }
    pdf_skip_space(cursor);
    if (cursor->position >= cursor->size) {
        return 0;
    }
    memset(out_value, 0, sizeof(*out_value));
    start = cursor->position;
    out_value->span.data = cursor->data;
    out_value->span.data_size = cursor->size;
    out_value->span.start = start;
    c = cursor->data[cursor->position];
    if (c == '/') {
        out_value->kind = PDF_VALUE_NAME;
        if (!pdf_parse_name(cursor, &out_value->span)) {
            return 0;
        }
    } else if (c == '[') {
        out_value->kind = PDF_VALUE_ARRAY;
        if (!pdf_parse_array(cursor, depth)) {
            return 0;
        }
        out_value->span.start = start;
        out_value->span.end = cursor->position;
    } else if (c == '<' && cursor->position + 1u < cursor->size &&
               cursor->data[cursor->position + 1u] == '<') {
        out_value->kind = PDF_VALUE_DICTIONARY;
        if (!pdf_parse_dictionary(cursor, depth)) {
            return 0;
        }
        out_value->span.start = start;
        out_value->span.end = cursor->position;
    } else if (c == '<') {
        ++cursor->position;
        while (cursor->position < cursor->size &&
               cursor->data[cursor->position] != '>') {
            ++cursor->position;
        }
        if (cursor->position >= cursor->size) {
            return 0;
        }
        ++cursor->position;
        out_value->kind = PDF_VALUE_OTHER;
        out_value->span.start = start;
        out_value->span.end = cursor->position;
    } else if (c == '(') {
        if (!pdf_skip_literal_string(cursor)) {
            return 0;
        }
        out_value->kind = PDF_VALUE_OTHER;
        out_value->span.start = start;
        out_value->span.end = cursor->position;
    } else if (c == ']' || c == '>' || c == ')') {
        return 0;
    } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        double number;
        int integer;
        uint64_t unsigned_number;
        if (!pdf_parse_number(cursor, &number, &integer, &unsigned_number)) {
            return 0;
        }
        out_value->kind = PDF_VALUE_NUMBER;
        out_value->number = number;
        out_value->span.start = start;
        out_value->span.end = cursor->position;
        if (integer && number >= 0.0 && unsigned_number <= UINT32_MAX) {
            PdfParserCursor lookahead = *cursor;
            double generation_number;
            int generation_integer;
            uint64_t generation_unsigned;
            pdf_skip_space(&lookahead);
            if (pdf_parse_number(&lookahead, &generation_number,
                                 &generation_integer, &generation_unsigned) &&
                generation_integer && generation_number >= 0.0 &&
                generation_unsigned <= UINT16_MAX) {
                pdf_skip_space(&lookahead);
                if (lookahead.position < lookahead.size &&
                    lookahead.data[lookahead.position] == 'R' &&
                    (lookahead.position + 1u == lookahead.size ||
                     pdf_is_delimiter(lookahead.data[lookahead.position + 1u]))) {
                    ++lookahead.position;
                    out_value->kind = PDF_VALUE_REFERENCE;
                    out_value->object_number = (uint32_t)unsigned_number;
                    out_value->generation = (uint32_t)generation_unsigned;
                    out_value->span.end = lookahead.position;
                    cursor->position = lookahead.position;
                }
            }
        }
    } else {
        while (cursor->position < cursor->size &&
               !pdf_is_delimiter(cursor->data[cursor->position])) {
            ++cursor->position;
        }
        if (cursor->position == start) {
            return 0;
        }
        out_value->kind = PDF_VALUE_OTHER;
        out_value->span.start = start;
        out_value->span.end = cursor->position;
    }
    return 1;
}

static int pdf_span_name_equals(const QwPdfParser *parser, PdfSpan span,
                                const char *name)
{
    size_t expected = strlen(name);
    (void)parser;
    if (!span.data || span.end < span.start || span.end > span.data_size ||
        span.end - span.start != expected + 1u ||
        span.data[span.start] != '/') {
        return 0;
    }
    return memcmp(span.data + span.start + 1u, name, expected) == 0;
}

static int pdf_dictionary_get(const QwPdfParser *parser, PdfSpan dictionary,
                              const char *key, PdfValue *out_value)
{
    PdfParserCursor cursor;
    (void)parser;
    if (!dictionary.data || dictionary.end > dictionary.data_size ||
        dictionary.start >= dictionary.end ||
        dictionary.end - dictionary.start < 4u ||
        dictionary.data[dictionary.start] != '<' ||
        dictionary.data[dictionary.start + 1u] != '<') {
        return 0;
    }
    cursor.data = dictionary.data;
    cursor.size = dictionary.data_size;
    cursor.position = dictionary.start + 2u;
    for (;;) {
        PdfSpan name;
        PdfValue value;
        pdf_skip_space(&cursor);
        if (cursor.position + 1u >= dictionary.end) {
            return 0;
        }
        if (dictionary.data[cursor.position] == '>' &&
            dictionary.data[cursor.position + 1u] == '>') {
            return 0;
        }
        if (!pdf_parse_name(&cursor, &name) ||
            !pdf_parse_value(&cursor, 0u, &value)) {
            return 0;
        }
        if (pdf_span_name_equals(parser, name, key)) {
            *out_value = value;
            return 1;
        }
    }
}

static int pdf_parse_name_value(const QwPdfParser *parser, PdfValue value,
                                const char *name)
{
    return value.kind == PDF_VALUE_NAME &&
           pdf_span_name_equals(parser, value.span, name);
}

static int pdf_value_as_unsigned(PdfValue value, uint64_t maximum,
                                 uint64_t *out_value)
{
    if (value.kind != PDF_VALUE_NUMBER || value.number < 0.0 ||
        !isfinite(value.number) || floor(value.number) != value.number ||
        value.number > (double)maximum) {
        return 0;
    }
    *out_value = (uint64_t)value.number;
    return 1;
}

static int pdf_stream_filter_name(QwPdfParser *parser,
                                 PdfSpan dictionary,
                                 int allow_indirect,
                                 PdfValue *out_filter)
{
    PdfValue filter;
    if (!pdf_dictionary_get(parser, dictionary, "Filter", &filter)) {
        memset(out_filter, 0, sizeof(*out_filter));
        out_filter->kind = PDF_VALUE_OTHER;
        return 1;
    }
    if (filter.kind == PDF_VALUE_REFERENCE) {
        if (!allow_indirect ||
            !pdf_resolve_value(parser, filter, &filter)) {
            return 0;
        }
    }
    if (filter.kind == PDF_VALUE_ARRAY) {
        PdfParserCursor cursor;
        PdfValue first;
        cursor.data = filter.span.data;
        cursor.size = filter.span.data_size;
        cursor.position = filter.span.start + 1u;
        if (!pdf_parse_value(&cursor, 0u, &first)) {
            return 0;
        }
        pdf_skip_space(&cursor);
        if (cursor.position >= filter.span.end ||
            filter.span.data[cursor.position] != ']' ||
            first.kind != PDF_VALUE_NAME) {
            return 0;
        }
        ++cursor.position;
        if (cursor.position != filter.span.end) {
            return 0;
        }
        filter = first;
    }
    if (filter.kind != PDF_VALUE_NAME) {
        return 0;
    }
    *out_filter = filter;
    return 1;
}

static int pdf_stream_predictor_parameters(QwPdfParser *parser,
                                           PdfSpan dictionary,
                                           int allow_indirect,
                                           unsigned *out_predictor,
                                           unsigned *out_colors,
                                           unsigned *out_bits,
                                           unsigned *out_columns)
{
    PdfValue parameters;
    PdfValue value;
    uint64_t number;
    *out_predictor = 1u;
    *out_colors = 1u;
    *out_bits = 8u;
    *out_columns = 1u;
    if (!pdf_dictionary_get(parser, dictionary, "DecodeParms", &parameters)) {
        return 1;
    }
    if (parameters.kind == PDF_VALUE_REFERENCE) {
        if (!allow_indirect ||
            !pdf_resolve_value(parser, parameters, &parameters)) {
            return 0;
        }
    }
    if (parameters.kind == PDF_VALUE_ARRAY) {
        PdfParserCursor cursor;
        cursor.data = parameters.span.data;
        cursor.size = parameters.span.data_size;
        cursor.position = parameters.span.start + 1u;
        pdf_skip_space(&cursor);
        if (cursor.position < parameters.span.end &&
            parameters.span.data[cursor.position] == ']') {
            return cursor.position + 1u == parameters.span.end;
        }
        if (!pdf_parse_value(&cursor, 0u, &parameters)) {
            return 0;
        }
        pdf_skip_space(&cursor);
        if (cursor.position >= parameters.span.end ||
            parameters.span.data[cursor.position] != ']') {
            return 0;
        }
        ++cursor.position;
        if (cursor.position != parameters.span.end) {
            return 0;
        }
        if (parameters.kind == PDF_VALUE_REFERENCE) {
            if (!allow_indirect ||
                !pdf_resolve_value(parser, parameters, &parameters)) {
                return 0;
            }
        }
    }
    if (parameters.kind == PDF_VALUE_OTHER &&
        parameters.span.end - parameters.span.start >= 4u &&
        memcmp(parameters.span.data + parameters.span.start, "null", 4u) == 0) {
        return 1;
    }
    if (parameters.kind != PDF_VALUE_DICTIONARY) {
        return 0;
    }
    if (pdf_dictionary_get(parser, parameters.span, "Predictor", &value)) {
        if (!pdf_value_as_unsigned(value, 15u, &number) || !number) return 0;
        *out_predictor = (unsigned)number;
    }
    if (pdf_dictionary_get(parser, parameters.span, "Colors", &value)) {
        if (!pdf_value_as_unsigned(value, 32u, &number) || !number) return 0;
        *out_colors = (unsigned)number;
    }
    if (pdf_dictionary_get(parser, parameters.span, "BitsPerComponent", &value)) {
        if (!pdf_value_as_unsigned(value, 16u, &number) || !number) return 0;
        *out_bits = (unsigned)number;
    }
    if (pdf_dictionary_get(parser, parameters.span, "Columns", &value)) {
        if (!pdf_value_as_unsigned(value, UINT32_MAX, &number) || !number) return 0;
        *out_columns = (unsigned)number;
    }
    return 1;
}

static int pdf_find_stream_bounds(QwPdfParser *parser,
                                  PdfSpan dictionary,
                                  int allow_indirect_length,
                                  size_t *out_start,
                                  size_t *out_size)
{
    PdfParserCursor cursor;
    size_t data_start;
    PdfValue length_value;
    uint64_t length;
    if (!dictionary.data || dictionary.data != parser->data ||
        dictionary.end > parser->size) {
        return 0;
    }
    cursor.data = parser->data;
    cursor.size = parser->size;
    cursor.position = dictionary.end;
    pdf_skip_space(&cursor);
    if (cursor.position + 6u > cursor.size ||
        memcmp(cursor.data + cursor.position, "stream", 6u) != 0 ||
        (cursor.position + 6u < cursor.size &&
         !pdf_is_delimiter(cursor.data[cursor.position + 6u]))) {
        return 0;
    }
    cursor.position += 6u;
    if (cursor.position >= cursor.size) return 0;
    if (cursor.data[cursor.position] == '\r') {
        ++cursor.position;
        if (cursor.position < cursor.size && cursor.data[cursor.position] == '\n') {
            ++cursor.position;
        }
    } else if (cursor.data[cursor.position] == '\n') {
        ++cursor.position;
    } else {
        return 0;
    }
    data_start = cursor.position;
    if (pdf_dictionary_get(parser, dictionary, "Length", &length_value)) {
        if (length_value.kind == PDF_VALUE_REFERENCE) {
            if (allow_indirect_length &&
                pdf_resolve_value(parser, length_value, &length_value)) {
                if (!pdf_value_as_unsigned(length_value, parser->size, &length)) {
                    return 0;
                }
                if (length <= parser->size - data_start) {
                    PdfParserCursor end_cursor = cursor;
                    end_cursor.position = data_start + (size_t)length;
                    pdf_skip_space(&end_cursor);
                    if (end_cursor.position + 9u <= end_cursor.size &&
                        memcmp(end_cursor.data + end_cursor.position,
                               "endstream", 9u) == 0) {
                        *out_start = data_start;
                        *out_size = (size_t)length;
                        return 1;
                    }
                }
            }
        } else if (pdf_value_as_unsigned(length_value, parser->size, &length) &&
                   length <= parser->size - data_start) {
            PdfParserCursor end_cursor = cursor;
            end_cursor.position = data_start + (size_t)length;
            pdf_skip_space(&end_cursor);
            if (end_cursor.position + 9u <= end_cursor.size &&
                memcmp(end_cursor.data + end_cursor.position,
                       "endstream", 9u) == 0) {
                *out_start = data_start;
                *out_size = (size_t)length;
                return 1;
            }
        }
    }
    for (size_t position = data_start; position + 9u <= parser->size; ++position) {
        if ((position == data_start || parser->data[position - 1u] == '\r' ||
             parser->data[position - 1u] == '\n') &&
            memcmp(parser->data + position, "endstream", 9u) == 0 &&
            (position + 9u == parser->size ||
             pdf_is_delimiter(parser->data[position + 9u]))) {
            size_t end = position;
            if (end > data_start && parser->data[end - 1u] == '\n') --end;
            if (end > data_start && parser->data[end - 1u] == '\r') --end;
            *out_start = data_start;
            *out_size = end - data_start;
            return 1;
        }
    }
    return 0;
}

static int pdf_decode_stream(QwPdfParser *parser,
                             PdfSpan dictionary,
                             size_t raw_offset,
                             size_t raw_size,
                             int allow_indirect_parameters,
                             uint8_t **out_data,
                             size_t *out_size)
{
    PdfValue filter;
    unsigned predictor;
    unsigned colors;
    unsigned bits_per_component;
    unsigned columns;
    uint8_t *decoded;
    if (raw_offset > parser->size || raw_size > parser->size - raw_offset ||
        raw_size > PDF_PARSER_MAX_DECODED_STREAM ||
        !pdf_stream_filter_name(parser, dictionary,
                                allow_indirect_parameters, &filter)) {
        return QW_PAGE_ERR_FORMAT;
    }
    if (filter.kind == PDF_VALUE_OTHER) {
        decoded = (uint8_t *)malloc(raw_size ? raw_size : 1u);
        if (!decoded) return QW_PAGE_ERR_OUT_OF_MEMORY;
        if (raw_size) memcpy(decoded, parser->data + raw_offset, raw_size);
        *out_data = decoded;
        *out_size = raw_size;
        return QW_PAGE_OK;
    }
    if (!pdf_span_name_equals(parser, filter.span, "FlateDecode") &&
        !pdf_span_name_equals(parser, filter.span, "Fl")) {
        return QW_PAGE_ERR_UNSUPPORTED;
    }
    if (!pdf_stream_predictor_parameters(parser, dictionary,
                                         allow_indirect_parameters,
                                         &predictor, &colors,
                                         &bits_per_component, &columns)) {
        return QW_PAGE_ERR_UNSUPPORTED;
    }
    if (!qw_pdf_flate_decode(parser->data + raw_offset, raw_size,
                             predictor, colors, bits_per_component, columns,
                             PDF_PARSER_MAX_DECODED_STREAM,
                             out_data, out_size)) {
        return QW_PAGE_ERR_FORMAT;
    }
    return QW_PAGE_OK;
}

static int pdf_xref_append(QwPdfParser *parser, const PdfXrefEntry *entry)
{
    PdfXrefEntry *grown;
    size_t capacity;
    if (parser->xref_count >= PDF_PARSER_MAX_XREF_ENTRIES) {
        return 0;
    }
    if (parser->xref_count == parser->xref_capacity) {
        capacity = parser->xref_capacity ? parser->xref_capacity * 2u : 128u;
        if (capacity > PDF_PARSER_MAX_XREF_ENTRIES) {
            capacity = PDF_PARSER_MAX_XREF_ENTRIES;
        }
        if (capacity > SIZE_MAX / sizeof(*grown)) {
            return 0;
        }
        grown = (PdfXrefEntry *)realloc(parser->xref,
                                        capacity * sizeof(*grown));
        if (!grown) {
            return 0;
        }
        parser->xref = grown;
        parser->xref_capacity = capacity;
    }
    parser->xref[parser->xref_count++] = *entry;
    return 1;
}

static int pdf_parse_trailer_dictionary(QwPdfParser *parser,
                                        PdfValue trailer_value,
                                        unsigned revision,
                                        uint64_t *out_prev,
                                        int *out_has_prev,
                                        uint64_t *out_xref_stream,
                                        int *out_has_xref_stream)
{
    PdfValue value;
    uint64_t number;
    if (trailer_value.kind != PDF_VALUE_DICTIONARY) {
        return QW_PAGE_ERR_FORMAT;
    }
    if (pdf_dictionary_get(parser, trailer_value.span, "XRefStm", &value)) {
        if (!pdf_value_as_unsigned(value, parser->size - 1u, &number)) {
            return QW_PAGE_ERR_FORMAT;
        }
        if (out_xref_stream) *out_xref_stream = number;
        if (out_has_xref_stream) *out_has_xref_stream = 1;
    } else if (out_has_xref_stream) {
        *out_has_xref_stream = 0;
    }
    if (pdf_dictionary_get(parser, trailer_value.span, "Root", &value)) {
        if (value.kind != PDF_VALUE_REFERENCE) {
            return QW_PAGE_ERR_FORMAT;
        }
        if (!parser->has_root) {
            parser->root_object = value.object_number;
            parser->root_generation = value.generation;
            parser->has_root = 1;
        }
    }
    if (pdf_dictionary_get(parser, trailer_value.span, "Encrypt", &value)) {
        parser->info.encrypted = 1;
    }
    if (pdf_dictionary_get(parser, trailer_value.span, "Prev", &value)) {
        if (!pdf_value_as_unsigned(value, parser->size - 1u, &number)) {
            return QW_PAGE_ERR_FORMAT;
        }
        if (out_prev) *out_prev = number;
        if (out_has_prev) *out_has_prev = 1;
    } else if (out_has_prev) {
        *out_has_prev = 0;
    }
    (void)revision;
    return QW_PAGE_OK;
}

static int pdf_parse_trailer(QwPdfParser *parser, PdfParserCursor *cursor,
                             unsigned revision, uint64_t *out_prev,
                             int *out_has_prev, uint64_t *out_xref_stream,
                             int *out_has_xref_stream)
{
    PdfValue trailer_value;
    if (!pdf_parse_value(cursor, 0u, &trailer_value)) {
        return QW_PAGE_ERR_FORMAT;
    }
    return pdf_parse_trailer_dictionary(parser, trailer_value, revision,
                                        out_prev, out_has_prev,
                                        out_xref_stream, out_has_xref_stream);
}

static int pdf_parse_unsigned_array(PdfValue value, uint64_t *out_values,
                                    size_t expected_count, uint64_t maximum)
{
    PdfParserCursor cursor;
    if (value.kind != PDF_VALUE_ARRAY || !value.span.data ||
        value.span.end > value.span.data_size) {
        return 0;
    }
    cursor.data = value.span.data;
    cursor.size = value.span.data_size;
    cursor.position = value.span.start + 1u;
    for (size_t i = 0; i < expected_count; ++i) {
        PdfValue item;
        if (!pdf_parse_value(&cursor, 0u, &item) ||
            !pdf_value_as_unsigned(item, maximum, &out_values[i])) {
            return 0;
        }
    }
    pdf_skip_space(&cursor);
    return cursor.position < value.span.end &&
           value.span.data[cursor.position] == ']' &&
           cursor.position + 1u == value.span.end;
}

static int pdf_read_big_endian_field(const uint8_t *data, size_t size,
                                     size_t *position, unsigned width,
                                     uint64_t *out_value)
{
    uint64_t value = 0;
    if (width > 8u || *position > size || width > size - *position) {
        return 0;
    }
    for (unsigned i = 0; i < width; ++i) {
        value = (value << 8) | data[(*position)++];
    }
    *out_value = value;
    return 1;
}

static int pdf_parse_xref_stream_range(QwPdfParser *parser,
                                       const uint8_t *decoded,
                                       size_t decoded_size,
                                       size_t *position,
                                       uint64_t first,
                                       uint64_t count,
                                       const unsigned widths[3],
                                       unsigned revision,
                                       unsigned priority)
{
    size_t row_size = (size_t)widths[0] + widths[1] + widths[2];
    if (!row_size || first > PDF_PARSER_MAX_OBJECT_NUMBER ||
        count > PDF_PARSER_MAX_OBJECT_NUMBER ||
        first + count > (uint64_t)PDF_PARSER_MAX_OBJECT_NUMBER + 1u ||
        count > PDF_PARSER_MAX_XREF_ENTRIES - parser->xref_count ||
        count > (decoded_size - (*position <= decoded_size ? *position : decoded_size)) /
                    row_size) {
        return QW_PAGE_ERR_FORMAT;
    }
    for (uint64_t i = 0; i < count; ++i) {
        PdfXrefEntry entry;
        uint64_t fields[3] = {1u, 0u, 0u};
        for (size_t field = 0; field < 3u; ++field) {
            if (widths[field] &&
                !pdf_read_big_endian_field(decoded, decoded_size, position,
                                           widths[field], &fields[field])) {
                return QW_PAGE_ERR_FORMAT;
            }
        }
        memset(&entry, 0, sizeof(entry));
        entry.object_number = (uint32_t)(first + i);
        entry.revision = revision;
        entry.priority = priority;
        entry.type = (unsigned)fields[0];
        if (entry.type == 0u) {
            if (fields[2] > UINT16_MAX) return QW_PAGE_ERR_FORMAT;
            entry.generation = (uint32_t)fields[2];
            entry.in_use = 0;
        } else if (entry.type == 1u) {
            if (fields[1] >= parser->size || fields[2] > UINT16_MAX) {
                return QW_PAGE_ERR_FORMAT;
            }
            entry.offset = (size_t)fields[1];
            entry.generation = (uint32_t)fields[2];
            entry.in_use = 1;
        } else if (entry.type == 2u) {
            if (fields[1] > PDF_PARSER_MAX_OBJECT_NUMBER ||
                fields[2] > UINT32_MAX) {
                return QW_PAGE_ERR_FORMAT;
            }
            entry.object_stream_number = (uint32_t)fields[1];
            entry.object_stream_index = (uint32_t)fields[2];
            entry.generation = 0u;
            entry.in_use = 1;
        } else {
            return QW_PAGE_ERR_UNSUPPORTED;
        }
        if (!pdf_xref_append(parser, &entry)) {
            return QW_PAGE_ERR_OUT_OF_MEMORY;
        }
    }
    return QW_PAGE_OK;
}

static int pdf_parse_xref_stream(QwPdfParser *parser, uint64_t xref_offset,
                                 unsigned revision, int hybrid,
                                 uint64_t *out_prev, int *out_has_prev)
{
    PdfParserCursor cursor;
    PdfValue dictionary;
    PdfValue value;
    uint64_t object_number;
    uint64_t generation;
    uint64_t size_value;
    uint64_t widths_value[3];
    unsigned widths[3];
    size_t raw_offset;
    size_t raw_size;
    uint8_t *decoded = NULL;
    size_t decoded_size = 0u;
    int rc;
    if (xref_offset >= parser->size ||
        parser->size - (size_t)xref_offset < 8u) {
        return QW_PAGE_ERR_FORMAT;
    }
    cursor.data = parser->data;
    cursor.size = parser->size;
    cursor.position = (size_t)xref_offset;
    if (!pdf_parse_unsigned(&cursor, &object_number) ||
        !pdf_parse_unsigned(&cursor, &generation) ||
        object_number > PDF_PARSER_MAX_OBJECT_NUMBER || generation > UINT16_MAX) {
        return QW_PAGE_ERR_FORMAT;
    }
    pdf_skip_space(&cursor);
    if (cursor.position + 3u > cursor.size ||
        memcmp(cursor.data + cursor.position, "obj", 3u) != 0 ||
        (cursor.position + 3u < cursor.size &&
         !pdf_is_delimiter(cursor.data[cursor.position + 3u]))) {
        return QW_PAGE_ERR_FORMAT;
    }
    cursor.position += 3u;
    if (!pdf_parse_value(&cursor, 0u, &dictionary) ||
        dictionary.kind != PDF_VALUE_DICTIONARY ||
        !pdf_dictionary_get(parser, dictionary.span, "Type", &value) ||
        !pdf_parse_name_value(parser, value, "XRef")) {
        return QW_PAGE_ERR_FORMAT;
    }
    rc = pdf_parse_trailer_dictionary(parser, dictionary, revision,
                                      out_prev, out_has_prev, NULL, NULL);
    if (rc != QW_PAGE_OK) return rc;
    if (!pdf_dictionary_get(parser, dictionary.span, "Size", &value) ||
        !pdf_value_as_unsigned(value,
                               (uint64_t)PDF_PARSER_MAX_OBJECT_NUMBER + 1u,
                               &size_value) ||
        !pdf_dictionary_get(parser, dictionary.span, "W", &value) ||
        !pdf_parse_unsigned_array(value, widths_value, 3u, 8u)) {
        return QW_PAGE_ERR_FORMAT;
    }
    if (object_number >= size_value) return QW_PAGE_ERR_FORMAT;
    for (size_t i = 0; i < 3u; ++i) widths[i] = (unsigned)widths_value[i];
    if ((size_t)widths[0] + widths[1] + widths[2] == 0u) {
        return QW_PAGE_ERR_FORMAT;
    }
    if (!pdf_find_stream_bounds(parser, dictionary.span, 0,
                                &raw_offset, &raw_size)) {
        return QW_PAGE_ERR_FORMAT;
    }
    rc = pdf_decode_stream(parser, dictionary.span, raw_offset, raw_size, 0,
                           &decoded, &decoded_size);
    if (rc != QW_PAGE_OK) return rc;
    {
        size_t position = 0u;
        PdfValue index_value;
        if (pdf_dictionary_get(parser, dictionary.span, "Index", &index_value)) {
            PdfParserCursor index_cursor;
            if (index_value.kind != PDF_VALUE_ARRAY) {
                free(decoded);
                return QW_PAGE_ERR_FORMAT;
            }
            index_cursor.data = index_value.span.data;
            index_cursor.size = index_value.span.data_size;
            index_cursor.position = index_value.span.start + 1u;
            for (;;) {
                uint64_t first;
                uint64_t count;
                pdf_skip_space(&index_cursor);
                if (index_cursor.position >= index_value.span.end) {
                    free(decoded);
                    return QW_PAGE_ERR_FORMAT;
                }
                if (index_value.span.data[index_cursor.position] == ']') {
                    ++index_cursor.position;
                    if (index_cursor.position != index_value.span.end) {
                        free(decoded);
                        return QW_PAGE_ERR_FORMAT;
                    }
                    break;
                }
                if (!pdf_parse_unsigned(&index_cursor, &first) ||
                    !pdf_parse_unsigned(&index_cursor, &count) ||
                    first > size_value || count > size_value - first) {
                    free(decoded);
                    return QW_PAGE_ERR_FORMAT;
                }
                rc = pdf_parse_xref_stream_range(parser, decoded, decoded_size,
                                                 &position, first, count, widths,
                                                 revision, hybrid ? 1u : 2u);
                if (rc != QW_PAGE_OK) {
                    free(decoded);
                    return rc;
                }
            }
        } else {
            rc = pdf_parse_xref_stream_range(parser, decoded, decoded_size,
                                             &position, 0u, size_value, widths,
                                             revision, hybrid ? 1u : 2u);
            if (rc != QW_PAGE_OK) {
                free(decoded);
                return rc;
            }
        }
        if (position != decoded_size) {
            free(decoded);
            return QW_PAGE_ERR_FORMAT;
        }
    }
    free(decoded);
    return QW_PAGE_OK;
}

static int pdf_parse_xref_section(QwPdfParser *parser, uint64_t xref_offset,
                                  unsigned revision, uint64_t *out_prev,
                                  int *out_has_prev)
{
    PdfParserCursor cursor;
    if (xref_offset >= parser->size || parser->size - (size_t)xref_offset < 4u ||
        memcmp(parser->data + (size_t)xref_offset, "xref", 4u) != 0) {
        return pdf_parse_xref_stream(parser, xref_offset, revision, 0,
                                     out_prev, out_has_prev);
    }
    cursor.data = parser->data;
    cursor.size = parser->size;
    cursor.position = (size_t)xref_offset + 4u;
    for (;;) {
        uint64_t first;
        uint64_t count;
        pdf_skip_space(&cursor);
        if (cursor.position + 7u <= cursor.size &&
            memcmp(cursor.data + cursor.position, "trailer", 7u) == 0 &&
            (cursor.position + 7u == cursor.size ||
             pdf_is_delimiter(cursor.data[cursor.position + 7u]))) {
            cursor.position += 7u;
            uint64_t xref_stream = 0u;
            int has_xref_stream = 0;
            int rc = pdf_parse_trailer(parser, &cursor, revision,
                                       out_prev, out_has_prev,
                                       &xref_stream, &has_xref_stream);
            if (rc != QW_PAGE_OK || !has_xref_stream) return rc;
            return pdf_parse_xref_stream(parser, xref_stream, revision, 1,
                                         NULL, NULL);
        }
        if (!pdf_parse_unsigned(&cursor, &first) ||
            !pdf_parse_unsigned(&cursor, &count) ||
            first > PDF_PARSER_MAX_OBJECT_NUMBER ||
        count > (uint64_t)PDF_PARSER_MAX_OBJECT_NUMBER + 1u ||
            first + count > (uint64_t)PDF_PARSER_MAX_OBJECT_NUMBER + 1u ||
            count > PDF_PARSER_MAX_XREF_ENTRIES - parser->xref_count) {
            return QW_PAGE_ERR_FORMAT;
        }
        for (uint64_t i = 0; i < count; ++i) {
            PdfXrefEntry entry;
            uint64_t offset;
            uint64_t generation;
            pdf_skip_space(&cursor);
            if (!pdf_parse_unsigned(&cursor, &offset) ||
                !pdf_parse_unsigned(&cursor, &generation) ||
                generation > UINT16_MAX) {
                return QW_PAGE_ERR_FORMAT;
            }
            pdf_skip_space(&cursor);
            if (cursor.position >= cursor.size ||
                (cursor.data[cursor.position] != 'n' &&
                 cursor.data[cursor.position] != 'f')) {
                return QW_PAGE_ERR_FORMAT;
            }
            memset(&entry, 0, sizeof(entry));
            entry.object_number = (uint32_t)(first + i);
            entry.generation = (uint32_t)generation;
            entry.offset = offset <= SIZE_MAX ? (size_t)offset : SIZE_MAX;
            entry.revision = revision;
            entry.in_use = cursor.data[cursor.position] == 'n';
            entry.type = entry.in_use ? 1u : 0u;
            entry.priority = 2u;
            if (entry.in_use && entry.offset >= parser->size) {
                return QW_PAGE_ERR_FORMAT;
            }
            if (!pdf_xref_append(parser, &entry)) {
                return QW_PAGE_ERR_OUT_OF_MEMORY;
            }
            while (cursor.position < cursor.size &&
                   cursor.data[cursor.position] != '\r' &&
                   cursor.data[cursor.position] != '\n') {
                ++cursor.position;
            }
        }
    }
}

static int pdf_xref_compare(const void *left, const void *right)
{
    const PdfXrefEntry *a = (const PdfXrefEntry *)left;
    const PdfXrefEntry *b = (const PdfXrefEntry *)right;
    if (a->object_number != b->object_number) {
        return a->object_number < b->object_number ? -1 : 1;
    }
    if (a->revision != b->revision) {
        return a->revision < b->revision ? -1 : 1;
    }
    if (a->priority != b->priority) {
        return a->priority > b->priority ? -1 : 1;
    }
    return 0;
}

static const PdfXrefEntry *pdf_xref_find(const QwPdfParser *parser,
                                         uint32_t object_number)
{
    size_t low = 0;
    size_t high = parser->xref_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (parser->xref[middle].object_number < object_number) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low >= parser->xref_count ||
        parser->xref[low].object_number != object_number) {
        return NULL;
    }
    return &parser->xref[low];
}

static int pdf_find_startxref(const QwPdfParser *parser, uint64_t *out_offset)
{
    static const uint8_t keyword[] = "startxref";
    size_t position = parser->size;
    while (position > 0u) {
        --position;
        if (position + sizeof(keyword) - 1u <= parser->size &&
            memcmp(parser->data + position, keyword,
                   sizeof(keyword) - 1u) == 0) {
            PdfParserCursor cursor;
            uint64_t offset;
            cursor.data = parser->data;
            cursor.size = parser->size;
            cursor.position = position + sizeof(keyword) - 1u;
            if (pdf_parse_unsigned(&cursor, &offset) && offset < parser->size) {
                *out_offset = offset;
                return 1;
            }
        }
    }
    return 0;
}

static int pdf_parse_version(QwPdfParser *parser)
{
    size_t limit = parser->size < 1024u ? parser->size : 1024u;
    for (size_t i = 0; i + 8u < limit; ++i) {
        if (memcmp(parser->data + i, "%PDF-", 5u) == 0 &&
            parser->data[i + 5u] >= '0' && parser->data[i + 5u] <= '9' &&
            parser->data[i + 6u] == '.' &&
            parser->data[i + 7u] >= '0' && parser->data[i + 7u] <= '9') {
            parser->info.version_major =
                (unsigned)(parser->data[i + 5u] - '0');
            parser->info.version_minor =
                (unsigned)(parser->data[i + 7u] - '0');
            return 1;
        }
    }
    return 0;
}

static int pdf_parse_xrefs(QwPdfParser *parser)
{
    uint64_t current;
    uint64_t visited[PDF_PARSER_MAX_XREF_SECTIONS];
    size_t visited_count = 0;
    int has_startxref = 0;
    if (!pdf_find_startxref(parser, &current)) {
        return QW_PAGE_ERR_FORMAT;
    }
    for (unsigned revision = 0; revision < PDF_PARSER_MAX_XREF_SECTIONS;
         ++revision) {
        uint64_t previous = 0;
        int has_previous = 0;
        int rc;
        for (size_t i = 0; i < visited_count; ++i) {
            if (visited[i] == current) {
                return QW_PAGE_ERR_FORMAT;
            }
        }
        visited[visited_count++] = current;
        rc = pdf_parse_xref_section(parser, current, revision,
                                    &previous, &has_previous);
        if (rc != QW_PAGE_OK) {
            return rc;
        }
        has_startxref = 1;
        if (!has_previous) {
            break;
        }
        current = previous;
        if (revision + 1u == PDF_PARSER_MAX_XREF_SECTIONS) {
            return QW_PAGE_ERR_FORMAT;
        }
    }
    if (!has_startxref || !parser->has_root || !parser->xref_count) {
        return QW_PAGE_ERR_FORMAT;
    }
    qsort(parser->xref, parser->xref_count, sizeof(*parser->xref),
          pdf_xref_compare);
    {
        size_t write = 0u;
        for (size_t read = 0u; read < parser->xref_count; ++read) {
            if (write &&
                parser->xref[write - 1u].object_number ==
                    parser->xref[read].object_number &&
                parser->xref[write - 1u].revision == parser->xref[read].revision) {
                continue;
            }
            if (write != read) parser->xref[write] = parser->xref[read];
            ++write;
        }
        parser->xref_count = write;
    }
    return QW_PAGE_OK;
}

static int pdf_object_value(QwPdfParser *parser, uint32_t object_number,
                            uint32_t generation, PdfValue *out_value)
{
    const PdfXrefEntry *entry = pdf_xref_find(parser, object_number);
    PdfParserCursor cursor;
    uint64_t parsed_object;
    uint64_t parsed_generation;
    if (!entry || !entry->in_use) {
        return 0;
    }
    if (entry->type == 2u) {
        PdfObjectStream *stream;
        size_t object_index = entry->object_stream_index;
        size_t object_start;
        size_t object_limit;
        if (generation != 0u ||
            !(stream = pdf_object_stream_load(parser,
                                               entry->object_stream_number)) ||
            object_index >= stream->object_count ||
            stream->object_numbers[object_index] != object_number) {
            return 0;
        }
        object_start = stream->first_object_offset +
                       stream->object_offsets[object_index];
        object_limit = object_index + 1u < stream->object_count
            ? stream->first_object_offset + stream->object_offsets[object_index + 1u]
            : stream->decoded_size;
        if (object_start >= object_limit || object_limit > stream->decoded_size) {
            return 0;
        }
        cursor.data = stream->decoded_data;
        cursor.size = object_limit;
        cursor.position = object_start;
        if (!pdf_parse_value(&cursor, 0u, out_value) ||
            out_value->span.end > object_limit) {
            return 0;
        }
        return 1;
    }
    if (entry->type != 1u || entry->generation != generation ||
        entry->offset >= parser->size) return 0;
    cursor.data = parser->data;
    cursor.size = parser->size;
    cursor.position = entry->offset;
    if (!pdf_parse_unsigned(&cursor, &parsed_object) ||
        !pdf_parse_unsigned(&cursor, &parsed_generation) ||
        parsed_object != object_number || parsed_generation != generation) {
        return 0;
    }
    pdf_skip_space(&cursor);
    if (cursor.position + 3u > cursor.size ||
        memcmp(cursor.data + cursor.position, "obj", 3u) != 0 ||
        (cursor.position + 3u < cursor.size &&
         !pdf_is_delimiter(cursor.data[cursor.position + 3u]))) {
        return 0;
    }
    cursor.position += 3u;
    return pdf_parse_value(&cursor, 0u, out_value);
}

static PdfObjectStream *pdf_object_stream_load(QwPdfParser *parser,
                                               uint32_t object_number)
{
    const PdfXrefEntry *entry = pdf_xref_find(parser, object_number);
    PdfValue dictionary;
    PdfValue value;
    uint64_t object_count_value;
    uint64_t first_value;
    uint8_t *decoded = NULL;
    size_t decoded_size = 0u;
    size_t raw_offset;
    size_t raw_size;
    uint32_t *object_numbers = NULL;
    size_t *object_offsets = NULL;
    size_t object_count;
    int rc;
    for (size_t i = 0; i < parser->object_stream_count; ++i) {
        if (parser->object_streams[i].object_number == object_number) {
            return &parser->object_streams[i];
        }
    }
    if (!entry || !entry->in_use || entry->type != 1u ||
        !pdf_object_value(parser, object_number, entry->generation, &dictionary) ||
        dictionary.kind != PDF_VALUE_DICTIONARY ||
        !pdf_dictionary_get(parser, dictionary.span, "Type", &value) ||
        !pdf_parse_name_value(parser, value, "ObjStm") ||
        !pdf_dictionary_get(parser, dictionary.span, "N", &value) ||
        !pdf_value_as_unsigned(value, PDF_PARSER_MAX_XREF_ENTRIES,
                               &object_count_value) ||
        !object_count_value ||
        !pdf_dictionary_get(parser, dictionary.span, "First", &value) ||
        !pdf_value_as_unsigned(value, PDF_PARSER_MAX_DECODED_STREAM,
                               &first_value)) {
        return NULL;
    }
    object_count = (size_t)object_count_value;
    if (object_count > SIZE_MAX / sizeof(*object_numbers) ||
        object_count > SIZE_MAX / sizeof(*object_offsets) ||
        !pdf_find_stream_bounds(parser, dictionary.span, 1,
                                &raw_offset, &raw_size)) {
        return NULL;
    }
    rc = pdf_decode_stream(parser, dictionary.span, raw_offset, raw_size, 1,
                           &decoded, &decoded_size);
    if (rc != QW_PAGE_OK || first_value > decoded_size ||
        decoded_size > PDF_PARSER_MAX_TOTAL_DECODED_STREAMS -
                           parser->total_decoded_stream_bytes) {
        free(decoded);
        return NULL;
    }
    object_numbers = (uint32_t *)malloc(object_count * sizeof(*object_numbers));
    object_offsets = (size_t *)malloc(object_count * sizeof(*object_offsets));
    if (!object_numbers || !object_offsets) {
        free(object_numbers);
        free(object_offsets);
        free(decoded);
        return NULL;
    }
    {
        PdfParserCursor header;
        header.data = decoded;
        header.size = (size_t)first_value;
        header.position = 0u;
        for (size_t i = 0; i < object_count; ++i) {
            uint64_t embedded_object;
            uint64_t object_offset;
            if (!pdf_parse_unsigned(&header, &embedded_object) ||
                !pdf_parse_unsigned(&header, &object_offset) ||
                !embedded_object ||
                embedded_object > PDF_PARSER_MAX_OBJECT_NUMBER ||
                object_offset > decoded_size - (size_t)first_value ||
                (i == 0u && object_offset != 0u) ||
                (i && object_offset <= object_offsets[i - 1u])) {
                free(object_numbers);
                free(object_offsets);
                free(decoded);
                return NULL;
            }
            object_numbers[i] = (uint32_t)embedded_object;
            object_offsets[i] = (size_t)object_offset;
        }
        pdf_skip_space(&header);
        if (header.position != header.size) {
            free(object_numbers);
            free(object_offsets);
            free(decoded);
            return NULL;
        }
    }
    if (parser->object_stream_count >= PDF_PARSER_MAX_OBJECT_STREAMS) {
        free(object_numbers);
        free(object_offsets);
        free(decoded);
        return NULL;
    }
    if (parser->object_stream_count == parser->object_stream_capacity) {
        size_t capacity = parser->object_stream_capacity
            ? parser->object_stream_capacity * 2u : 8u;
        PdfObjectStream *grown;
        if (capacity > PDF_PARSER_MAX_OBJECT_STREAMS) {
            capacity = PDF_PARSER_MAX_OBJECT_STREAMS;
        }
        if (capacity > SIZE_MAX / sizeof(*grown)) {
            free(object_numbers);
            free(object_offsets);
            free(decoded);
            return NULL;
        }
        grown = (PdfObjectStream *)realloc(parser->object_streams,
                                           capacity * sizeof(*grown));
        if (!grown) {
            free(object_numbers);
            free(object_offsets);
            free(decoded);
            return NULL;
        }
        parser->object_streams = grown;
        parser->object_stream_capacity = capacity;
    }
    {
        PdfObjectStream *stream =
            &parser->object_streams[parser->object_stream_count++];
        stream->object_number = object_number;
        stream->object_numbers = object_numbers;
        stream->object_offsets = object_offsets;
        stream->object_count = object_count;
        stream->first_object_offset = (size_t)first_value;
        stream->decoded_data = decoded;
        stream->decoded_size = decoded_size;
        parser->total_decoded_stream_bytes += decoded_size;
        return stream;
    }
}

static int pdf_resolve_value(QwPdfParser *parser, PdfValue value,
                             PdfValue *out_value)
{
    if (value.kind != PDF_VALUE_REFERENCE) {
        *out_value = value;
        return 1;
    }
    return pdf_object_value(parser, value.object_number, value.generation,
                            out_value);
}

static int pdf_read_rect(QwPdfParser *parser, PdfValue value,
                         QwPdfRect *out_rect)
{
    PdfValue resolved;
    PdfParserCursor cursor;
    double values[4];
    int integer;
    uint64_t unsigned_number;
    if (!pdf_resolve_value(parser, value, &resolved) ||
        resolved.kind != PDF_VALUE_ARRAY) {
        return 0;
    }
    cursor.data = resolved.span.data;
    cursor.size = resolved.span.data_size;
    cursor.position = resolved.span.start + 1u;
    for (size_t i = 0; i < 4u; ++i) {
        if (!pdf_parse_number(&cursor, &values[i], &integer, &unsigned_number)) {
            return 0;
        }
    }
    pdf_skip_space(&cursor);
    if (cursor.position >= resolved.span.end ||
        resolved.span.data[cursor.position] != ']' ||
        !isfinite(values[0]) || !isfinite(values[1]) ||
        !isfinite(values[2]) || !isfinite(values[3]) ||
        values[2] <= values[0] || values[3] <= values[1]) {
        return 0;
    }
    out_rect->x0 = values[0];
    out_rect->y0 = values[1];
    out_rect->x1 = values[2];
    out_rect->y1 = values[3];
    return 1;
}

static int pdf_read_rotation(QwPdfParser *parser, PdfValue value,
                            int *out_rotation)
{
    PdfValue resolved;
    double rotation;
    if (!pdf_resolve_value(parser, value, &resolved) ||
        resolved.kind != PDF_VALUE_NUMBER ||
        floor(resolved.number) != resolved.number ||
        resolved.number < (double)INT32_MIN ||
        resolved.number > (double)INT32_MAX) {
        return 0;
    }
    rotation = fmod(resolved.number, 360.0);
    if (rotation < 0.0) {
        rotation += 360.0;
    }
    if (fmod(rotation, 90.0) != 0.0) {
        return 0;
    }
    *out_rotation = (int)rotation;
    return 1;
}

static int pdf_page_append(QwPdfParser *parser,
                           const PdfInheritedPageInfo *inherited)
{
    QwPdfParserPageInfo *grown;
    size_t capacity;
    if (!inherited->has_media_box || parser->page_count >= PDF_PARSER_MAX_PAGES) {
        return 0;
    }
    if (parser->page_count == parser->page_capacity) {
        capacity = parser->page_capacity ? parser->page_capacity * 2u : 32u;
        if (capacity > PDF_PARSER_MAX_PAGES) {
            capacity = PDF_PARSER_MAX_PAGES;
        }
        if (capacity > SIZE_MAX / sizeof(*grown)) {
            return 0;
        }
        grown = (QwPdfParserPageInfo *)realloc(parser->pages,
                                                capacity * sizeof(*grown));
        if (!grown) {
            return 0;
        }
        parser->pages = grown;
        parser->page_capacity = capacity;
    }
    parser->pages[parser->page_count].media_box = inherited->media_box;
    parser->pages[parser->page_count].crop_box = inherited->has_crop_box
        ? inherited->crop_box : inherited->media_box;
    parser->pages[parser->page_count].rotation = inherited->has_rotation
        ? inherited->rotation : 0;
    ++parser->page_count;
    return 1;
}

static int pdf_page_walk_node(PdfPageWalk *walk, uint32_t object_number,
                              uint32_t generation,
                              PdfInheritedPageInfo inherited)
{
    QwPdfParser *parser = walk->parser;
    PdfValue object;
    PdfValue value;
    PdfValue type;
    PdfValue kids;
    int has_type = 0;
    int is_pages_node = 0;
    if (walk->depth >= PDF_PARSER_MAX_PAGE_DEPTH) {
        return 0;
    }
    for (size_t i = 0; i < walk->depth; ++i) {
        if (walk->stack[i] == object_number) {
            return 0;
        }
    }
    if (!pdf_object_value(parser, object_number, generation, &object) ||
        object.kind != PDF_VALUE_DICTIONARY) {
        return 0;
    }
    walk->stack[walk->depth++] = object_number;

    if (pdf_dictionary_get(parser, object.span, "MediaBox", &value)) {
        if (!pdf_read_rect(parser, value, &inherited.media_box)) {
            --walk->depth;
            return 0;
        }
        inherited.has_media_box = 1;
    }
    if (pdf_dictionary_get(parser, object.span, "CropBox", &value)) {
        if (!pdf_read_rect(parser, value, &inherited.crop_box)) {
            --walk->depth;
            return 0;
        }
        inherited.has_crop_box = 1;
    }
    if (pdf_dictionary_get(parser, object.span, "Rotate", &value)) {
        if (!pdf_read_rotation(parser, value, &inherited.rotation)) {
            --walk->depth;
            return 0;
        }
        inherited.has_rotation = 1;
    }
    if (pdf_dictionary_get(parser, object.span, "Type", &type)) {
        PdfValue resolved_type;
        if (!pdf_resolve_value(parser, type, &resolved_type)) {
            --walk->depth;
            return 0;
        }
        type = resolved_type;
        has_type = 1;
        is_pages_node = pdf_parse_name_value(parser, type, "Pages");
    }
    if (pdf_dictionary_get(parser, object.span, "Kids", &value)) {
        PdfParserCursor cursor;
        if (!is_pages_node || !pdf_resolve_value(parser, value, &kids) ||
            kids.kind != PDF_VALUE_ARRAY) {
            --walk->depth;
            return 0;
        }
        cursor.data = kids.span.data;
        cursor.size = kids.span.data_size;
        cursor.position = kids.span.start + 1u;
        for (;;) {
            PdfValue kid;
            pdf_skip_space(&cursor);
            if (cursor.position >= kids.span.end) {
                --walk->depth;
                return 0;
            }
            if (kids.span.data[cursor.position] == ']') {
                ++cursor.position;
                break;
            }
            if (!pdf_parse_value(&cursor, 0u, &kid) ||
                kid.kind != PDF_VALUE_REFERENCE ||
                !pdf_page_walk_node(walk, kid.object_number, kid.generation,
                                    inherited)) {
                --walk->depth;
                return 0;
            }
        }
    } else {
        if (has_type && !pdf_parse_name_value(parser, type, "Page")) {
            --walk->depth;
            return 0;
        }
        if (!pdf_page_append(parser, &inherited)) {
            --walk->depth;
            return 0;
        }
    }
    --walk->depth;
    return 1;
}

static int pdf_parse_page_tree(QwPdfParser *parser)
{
    PdfValue catalog;
    PdfValue pages_root;
    PdfPageWalk walk;
    if (!pdf_object_value(parser, parser->root_object,
                          parser->root_generation, &catalog) ||
        catalog.kind != PDF_VALUE_DICTIONARY ||
        !pdf_dictionary_get(parser, catalog.span, "Pages", &pages_root) ||
        pages_root.kind != PDF_VALUE_REFERENCE) {
        return QW_PAGE_ERR_FORMAT;
    }
    memset(&walk, 0, sizeof(walk));
    walk.parser = parser;
    if (!pdf_page_walk_node(&walk, pages_root.object_number,
                            pages_root.generation, (PdfInheritedPageInfo){0}) ||
        !parser->page_count) {
        return QW_PAGE_ERR_FORMAT;
    }
    parser->info.page_count = parser->page_count;
    return QW_PAGE_OK;
}

int qw_pdf_parser_open(const uint8_t *data, size_t size,
                       QwPdfParser **out_parser)
{
    QwPdfParser *parser;
    int rc;
    if (!data || !size || !out_parser) {
        return QW_PAGE_ERR_INVALID_ARGUMENT;
    }
    *out_parser = NULL;
    if (size < 8u) {
        return QW_PAGE_ERR_FORMAT;
    }
    parser = (QwPdfParser *)calloc(1u, sizeof(*parser));
    if (!parser) {
        return QW_PAGE_ERR_OUT_OF_MEMORY;
    }
    parser->data = data;
    parser->size = size;
    if (!pdf_parse_version(parser)) {
        qw_pdf_parser_close(parser);
        return QW_PAGE_ERR_FORMAT;
    }
    rc = pdf_parse_xrefs(parser);
    if (rc == QW_PAGE_OK) {
        rc = pdf_parse_page_tree(parser);
    }
    if (rc != QW_PAGE_OK) {
        qw_pdf_parser_close(parser);
        return rc;
    }
    *out_parser = parser;
    return QW_PAGE_OK;
}

int qw_pdf_parser_get_info(const QwPdfParser *parser,
                           QwPdfParserInfo *out_info)
{
    if (!parser || !out_info) {
        return QW_PAGE_ERR_INVALID_ARGUMENT;
    }
    *out_info = parser->info;
    return QW_PAGE_OK;
}

int qw_pdf_parser_get_page_info(const QwPdfParser *parser,
                                size_t page_index,
                                QwPdfParserPageInfo *out_info)
{
    if (!parser || !out_info) {
        return QW_PAGE_ERR_INVALID_ARGUMENT;
    }
    if (page_index >= parser->page_count) {
        return QW_PAGE_ERR_FORMAT;
    }
    *out_info = parser->pages[page_index];
    return QW_PAGE_OK;
}

void qw_pdf_parser_close(QwPdfParser *parser)
{
    if (!parser) {
        return;
    }
    for (size_t i = 0; i < parser->object_stream_count; ++i) {
        free(parser->object_streams[i].object_numbers);
        free(parser->object_streams[i].object_offsets);
        free(parser->object_streams[i].decoded_data);
    }
    free(parser->object_streams);
    free(parser->xref);
    free(parser->pages);
    free(parser);
}

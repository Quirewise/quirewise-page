#include "pdf_stream_codec.h"

#include <quirewise/compression.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int pdf_predictor_parameters(unsigned colors,
                                    unsigned bits_per_component,
                                    unsigned columns,
                                    size_t *out_row_size,
                                    size_t *out_bytes_per_pixel)
{
    size_t samples;
    size_t bits;
    if (!colors || colors > 32u || !columns ||
        (bits_per_component != 1u && bits_per_component != 2u &&
         bits_per_component != 4u && bits_per_component != 8u &&
         bits_per_component != 16u) ||
        (size_t)colors > SIZE_MAX / (size_t)columns) {
        return 0;
    }
    samples = (size_t)colors * (size_t)columns;
    if (samples > SIZE_MAX / bits_per_component) {
        return 0;
    }
    bits = samples * bits_per_component;
    if (bits > SIZE_MAX - 7u) {
        return 0;
    }
    *out_row_size = (bits + 7u) / 8u;
    *out_bytes_per_pixel =
        ((size_t)colors * bits_per_component + 7u) / 8u;
    return *out_row_size != 0u && *out_bytes_per_pixel != 0u;
}

static uint32_t pdf_read_packed_sample(const uint8_t *row,
                                       size_t sample_index,
                                       unsigned bits_per_component)
{
    size_t bit_offset = sample_index * bits_per_component;
    size_t byte_offset = bit_offset >> 3;
    if (bits_per_component == 16u) {
        byte_offset = sample_index * 2u;
        return ((uint32_t)row[byte_offset] << 8) | row[byte_offset + 1u];
    }
    unsigned bit_in_byte = (unsigned)(bit_offset & 7u);
    unsigned shift = 8u - bits_per_component - bit_in_byte;
    uint32_t mask = (1u << bits_per_component) - 1u;
    return ((uint32_t)row[byte_offset] >> shift) & mask;
}

static void pdf_write_packed_sample(uint8_t *row,
                                    size_t sample_index,
                                    unsigned bits_per_component,
                                    uint32_t value)
{
    size_t bit_offset = sample_index * bits_per_component;
    size_t byte_offset = bit_offset >> 3;
    if (bits_per_component == 16u) {
        byte_offset = sample_index * 2u;
        row[byte_offset] = (uint8_t)(value >> 8);
        row[byte_offset + 1u] = (uint8_t)value;
        return;
    }
    unsigned bit_in_byte = (unsigned)(bit_offset & 7u);
    unsigned shift = 8u - bits_per_component - bit_in_byte;
    uint32_t mask = ((1u << bits_per_component) - 1u) << shift;
    row[byte_offset] = (uint8_t)(((uint32_t)row[byte_offset] & ~mask) |
                                 ((value << shift) & mask));
}

static int pdf_apply_tiff_predictor(uint8_t *data,
                                    size_t size,
                                    unsigned colors,
                                    unsigned bits_per_component,
                                    unsigned columns,
                                    size_t row_size)
{
    size_t sample_count = (size_t)colors * columns;
    uint32_t mask = (1u << bits_per_component) - 1u;
    if (size % row_size != 0u) {
        return 0;
    }
    for (size_t row_offset = 0; row_offset < size; row_offset += row_size) {
        uint8_t *row = data + row_offset;
        for (size_t sample = colors; sample < sample_count; ++sample) {
            uint32_t current = pdf_read_packed_sample(row, sample,
                                                      bits_per_component);
            uint32_t left = pdf_read_packed_sample(row, sample - colors,
                                                   bits_per_component);
            pdf_write_packed_sample(row, sample, bits_per_component,
                                    (current + left) & mask);
        }
    }
    return 1;
}

static int pdf_paeth_predictor(int left, int up, int upper_left)
{
    int estimate = left + up - upper_left;
    int distance_left = abs(estimate - left);
    int distance_up = abs(estimate - up);
    int distance_upper_left = abs(estimate - upper_left);
    if (distance_left <= distance_up && distance_left <= distance_upper_left) {
        return left;
    }
    if (distance_up <= distance_upper_left) {
        return up;
    }
    return upper_left;
}

static int pdf_apply_png_predictor(uint8_t *data,
                                   size_t *size,
                                   unsigned predictor,
                                   size_t row_size,
                                   size_t bytes_per_pixel)
{
    size_t encoded_row_size;
    size_t row_count;
    if (row_size == SIZE_MAX) {
        return 0;
    }
    encoded_row_size = row_size + 1u;
    if (!encoded_row_size || *size % encoded_row_size != 0u) {
        return 0;
    }
    (void)predictor;
    row_count = *size / encoded_row_size;
    for (size_t row = 0; row < row_count; ++row) {
        size_t source_offset = row * encoded_row_size;
        size_t output_offset = row * row_size;
        unsigned filter = data[source_offset];
        if (filter > 4u) {
            return 0;
        }
        memmove(data + output_offset, data + source_offset + 1u, row_size);
        for (size_t x = 0; x < row_size; ++x) {
            int raw = data[output_offset + x];
            int left = x >= bytes_per_pixel
                ? data[output_offset + x - bytes_per_pixel] : 0;
            int up = row ? data[output_offset - row_size + x] : 0;
            int upper_left = row && x >= bytes_per_pixel
                ? data[output_offset - row_size + x - bytes_per_pixel] : 0;
            switch (filter) {
            case 0u:
                break;
            case 1u:
                raw += left;
                break;
            case 2u:
                raw += up;
                break;
            case 3u:
                raw += (left + up) / 2;
                break;
            case 4u:
                raw += pdf_paeth_predictor(left, up, upper_left);
                break;
            default:
                return 0;
            }
            data[output_offset + x] = (uint8_t)raw;
        }
    }
    *size = row_count * row_size;
    return 1;
}

int qw_pdf_flate_decode(const uint8_t *input,
                        size_t input_size,
                        unsigned predictor,
                        unsigned colors,
                        unsigned bits_per_component,
                        unsigned columns,
                        size_t max_output_size,
                        uint8_t **out_data,
                        size_t *out_size)
{
    uint8_t *output = NULL;
    size_t capacity;
    size_t output_size = 0u;
    size_t row_size = 0u;
    size_t bytes_per_pixel = 0u;
    int ok = 0;
    if (!out_data || !out_size || (!input && input_size) ||
        input_size > UINT_MAX || !max_output_size || predictor < 1u ||
        predictor > 15u) {
        return 0;
    }
    *out_data = NULL;
    *out_size = 0u;
    if (predictor > 1u &&
        !pdf_predictor_parameters(colors, bits_per_component, columns,
                                  &row_size, &bytes_per_pixel)) {
        return 0;
    }
    capacity = max_output_size < 16384u ? max_output_size : 16384u;
    output = (uint8_t *)malloc(capacity ? capacity : 1u);
    if (!output) {
        return 0;
    }
    for (;;) {
        QwCompressionInflateStatus inflate_status =
            qw_compression_inflate_zlib_bounded(input, input_size, output,
                                                capacity, &output_size);
        if (inflate_status == QW_COMPRESSION_INFLATE_OK) {
            ok = 1;
            break;
        }
        if (inflate_status != QW_COMPRESSION_INFLATE_OUTPUT_FULL ||
            capacity == max_output_size) {
            break;
        }
        {
            size_t grown_capacity = capacity > max_output_size / 2u
                ? max_output_size : capacity * 2u;
            uint8_t *grown;
            if (grown_capacity <= capacity) {
                break;
            }
            grown = (uint8_t *)realloc(output, grown_capacity);
            if (!grown) {
                goto cleanup;
            }
            output = grown;
            capacity = grown_capacity;
        }
    }
    if (!ok) {
        goto cleanup;
    }
    if (predictor == 2u) {
        if (!pdf_apply_tiff_predictor(output, output_size, colors,
                                      bits_per_component, columns, row_size)) {
            goto cleanup;
        }
    } else if (predictor >= 10u) {
        if (!pdf_apply_png_predictor(output, &output_size, predictor,
                                     row_size, bytes_per_pixel)) {
            goto cleanup;
        }
    } else if (predictor != 1u) {
        goto cleanup;
    }
    *out_data = output;
    *out_size = output_size;
    return 1;

cleanup:
    free(output);
    return 0;
}

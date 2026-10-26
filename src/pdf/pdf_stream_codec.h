#ifndef QUIREWISE_PDF_STREAM_CODEC_H
#define QUIREWISE_PDF_STREAM_CODEC_H

#include <stddef.h>
#include <stdint.h>

int qw_pdf_flate_decode(const uint8_t *input,
                        size_t input_size,
                        unsigned predictor,
                        unsigned colors,
                        unsigned bits_per_component,
                        unsigned columns,
                        size_t max_output_size,
                        uint8_t **out_data,
                        size_t *out_size);

#endif

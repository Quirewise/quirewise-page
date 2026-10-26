#ifndef QUIREWISE_TIFF_DECODE_H
#define QUIREWISE_TIFF_DECODE_H

#include "quirewise/document.h"

#include <stddef.h>
#include <stdint.h>

int qw_tiff_page_count(const uint8_t *data, size_t size, size_t *out_count);
int qw_tiff_probe(const void *data, size_t size);
int qw_tiff_decode_page(const uint8_t *data,
                        size_t size,
                        size_t page_index,
                        QwPageImage *out_page);

#endif

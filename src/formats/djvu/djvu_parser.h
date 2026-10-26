#ifndef QUIREWISE_DJVU_PARSER_H
#define QUIREWISE_DJVU_PARSER_H

#include <stddef.h>

int qw_djvu_probe(const void *data, size_t size);
int qw_djvu_parse_page_count(const void *data,
                             size_t size,
                             size_t *out_page_count);

#endif

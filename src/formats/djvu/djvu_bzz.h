/* First-party DjVu BZZ decoder (the compressed byte stream used by TXTz and
 * annotation chunks). */

#ifndef QUIREWISE_PAGE_DJVU_BZZ_H
#define QUIREWISE_PAGE_DJVU_BZZ_H

#include <stddef.h>
#include <stdint.h>

enum {
    QW_DJVU_BZZ_OK = 0,
    QW_DJVU_BZZ_ERR_FORMAT = -1,
    QW_DJVU_BZZ_ERR_MEMORY = -2,
    QW_DJVU_BZZ_ERR_LIMIT = -3
};

/* Decode a complete BZZ stream. On success *out_data points at a malloc()ed
 * buffer of *out_size bytes (NUL-terminated for convenience); the caller frees
 * it with free(). max_output bounds the decompressed size. */
int qw_djvu_bzz_decode(const uint8_t *data, size_t size,
                       uint8_t **out_data, size_t *out_size,
                       size_t max_output);

#endif

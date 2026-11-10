/* First-party IFF container reader for DjVu files.
 *
 * A DjVu file is an IFF FORM: "AT&TFORM", a 32-bit big-endian payload length,
 * a 4-byte form type ("DJVU" or "DJVM"), then a sequence of chunks. Each chunk
 * has a 4-byte id, a 32-bit big-endian length and its payload, padded to an
 * even length. Multi-page files store one nested "FORM" chunk per page. */

#ifndef QUIREWISE_PAGE_DJVU_IFF_H
#define QUIREWISE_PAGE_DJVU_IFF_H

#include <stddef.h>
#include <stdint.h>

typedef struct QwDjvuIffForm {
    const uint8_t *data; /* first chunk (form type already consumed) */
    size_t size;         /* chunk area length */
    char type[5];        /* "DJVU" or "DJVM" */
} QwDjvuIffForm;

typedef struct QwDjvuIffChunk {
    const uint8_t *data;
    size_t size;
    char id[5];
} QwDjvuIffChunk;

/* Parse a complete FORM header. Returns 0 on success, -1 on malformed input. */
int qw_djvu_iff_form(const uint8_t *data, size_t size, QwDjvuIffForm *out);

/* Wrap a nested FORM chunk payload (no AT&TFORM header inside DJVM). */
int qw_djvu_iff_nested(const QwDjvuIffChunk *chunk, QwDjvuIffForm *out);

/* Iterate chunks. Callers start with *cursor = 0; returns 1 on a chunk,
 * 0 at end of form, -1 on a malformed chunk. */
int qw_djvu_iff_next(const QwDjvuIffForm *form, size_t *cursor,
                     QwDjvuIffChunk *out);

/* Find the first chunk with the given 4-byte id. */
int qw_djvu_iff_find(const QwDjvuIffForm *form, const char *id,
                     QwDjvuIffChunk *out);

#endif

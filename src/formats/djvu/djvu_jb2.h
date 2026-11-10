/* First-party DjVu JB2 bitonal decoder (decode side only).
 *
 * JB2 encodes a page as a set of shape bitmaps (coded with the ZP arithmetic
 * coder and a context model over neighbouring pixels) plus blit records that
 * place the shapes on the page. This module implements the record layer and
 * bitmap coders needed by Sjbz/Djbz chunks; rendering is provided as a
 * plain 8-bit gray map. */

#ifndef QUIREWISE_PAGE_DJVU_JB2_H
#define QUIREWISE_PAGE_DJVU_JB2_H

#include <stddef.h>
#include <stdint.h>

struct QwDjvuJb2Image;

int qw_djvu_jb2_decode(const uint8_t *data, size_t size,
                       struct QwDjvuJb2Image **out_image);
void qw_djvu_jb2_free(struct QwDjvuJb2Image *image);
int qw_djvu_jb2_image_size(const struct QwDjvuJb2Image *image, int *width,
                           int *height);
/* Render the page into a caller-owned GRAY8 buffer (white 255, ink 0).
 * stride and height describe the destination. */
int qw_djvu_jb2_render_gray8(const struct QwDjvuJb2Image *image,
                             uint8_t *pixels, size_t stride, size_t height);

#endif

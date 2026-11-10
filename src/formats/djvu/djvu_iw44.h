/* First-party DjVu IW44 wavelet decoder (decode side only).
 *
 * A BG44/FG44 chunk carries a slice of a wavelet-coded plane. Chunks with
 * increasing serial numbers extend the same image; colour images carry three
 * planes (Y, Cb, Cr) whose Cb/Cr planes may be coded at half resolution. */

#ifndef QUIREWISE_PAGE_DJVU_IW44_H
#define QUIREWISE_PAGE_DJVU_IW44_H

#include <stddef.h>
#include <stdint.h>

extern const uint16_t qw_djvu_iw44_zigzag[1024];

struct QwDjvuIw44Image;

/* Decode an ordered sequence of BG44/FG44 chunk payloads. */
int qw_djvu_iw44_decode_chunks(struct QwDjvuIw44Image **out_image,
                               const uint8_t *const *chunks,
                               const size_t *sizes, size_t count);
void qw_djvu_iw44_free(struct QwDjvuIw44Image *image);
int qw_djvu_iw44_size(const struct QwDjvuIw44Image *image, int *width,
                      int *height);
int qw_djvu_iw44_is_color(const struct QwDjvuIw44Image *image);
/* Render to interleaved RGB24, top row first (w*h*3 bytes). */
int qw_djvu_iw44_render_rgb(const struct QwDjvuIw44Image *image,
                            uint8_t *rgb);
/* Render to a single 8-bit gray plane, top row first (w*h bytes). */
int qw_djvu_iw44_render_gray(const struct QwDjvuIw44Image *image,
                             uint8_t *gray);

#endif

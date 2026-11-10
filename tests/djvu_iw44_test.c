/* Unit tests for the first-party IW44 wavelet decoder.
 *
 * photo.djvu carries three BG44 chunks; the rendered page is compared
 * byte-for-byte with photo.ref.ppm (produced by the previous decoder). */

#include "djvu_iff.h"
#include "djvu_iw44.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_DJVU_FIXTURES
#define QUIREWISE_DJVU_FIXTURES "tests/data/djvu"
#endif

static unsigned char *read_file(const char *name, size_t *out_size)
{
    char path[1024];
    FILE *fh;
    long size;
    unsigned char *data;

    snprintf(path, sizeof(path), "%s/%s", QUIREWISE_DJVU_FIXTURES, name);
    fh = fopen(path, "rb");
    if (!fh) return NULL;
    if (fseek(fh, 0, SEEK_END) != 0) { fclose(fh); return NULL; }
    size = ftell(fh);
    if (size <= 0) { fclose(fh); return NULL; }
    if (fseek(fh, 0, SEEK_SET) != 0) { fclose(fh); return NULL; }
    data = (unsigned char *)malloc((size_t)size);
    if (!data) { fclose(fh); return NULL; }
    if (fread(data, 1, (size_t)size, fh) != (size_t)size) {
        free(data);
        fclose(fh);
        return NULL;
    }
    fclose(fh);
    *out_size = (size_t)size;
    return data;
}

static const unsigned char *pnm_pixels(const unsigned char *data, size_t size,
                                       char magic0, char magic1,
                                       int *width, int *height,
                                       size_t *pixel_size, size_t *channels)
{
    size_t pos = 0;
    int fields = 0;
    int values[3] = { 0, 0, 0 };

    if (size < 2 || data[0] != magic0 || data[1] != magic1) return NULL;
    pos = 2;
    while (pos < size && fields < 3) {
        while (pos < size && (data[pos] == ' ' || data[pos] == '\n' ||
                              data[pos] == '\r' || data[pos] == '\t')) {
            ++pos;
        }
        if (pos >= size) return NULL;
        if (data[pos] == '#') {
            while (pos < size && data[pos] != '\n') ++pos;
            continue;
        }
        values[fields] = 0;
        while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
            values[fields] = values[fields] * 10 + (data[pos] - '0');
            ++pos;
        }
        ++fields;
    }
    if (fields != 3 || pos >= size) return NULL;
    ++pos;
    *width = values[0];
    *height = values[1];
    *channels = (magic1 == '6') ? 3u : 1u;
    *pixel_size = (size_t)values[0] * (size_t)values[1] * *channels;
    if (pos + *pixel_size > size) return NULL;
    return data + pos;
}

static int load_bg44(const char *name, const uint8_t **chunks,
                     size_t *sizes, size_t *count, unsigned char **file_out)
{
    size_t file_size = 0;
    unsigned char *file = read_file(name, &file_size);
    QwDjvuIffForm form;
    size_t cursor = 0;
    QwDjvuIffChunk chunk;
    size_t n = 0;

    if (!file) return -1;
    if (qw_djvu_iff_form(file, file_size, &form) != 0) {
        free(file);
        return -1;
    }
    while (n < 8 && qw_djvu_iff_next(&form, &cursor, &chunk) == 1) {
        if (strcmp(chunk.id, "BG44") == 0) {
            chunks[n] = chunk.data;
            sizes[n] = chunk.size;
            ++n;
        }
    }
    if (n == 0) {
        free(file);
        return -1;
    }
    *count = n;
    *file_out = file;
    return 0;
}

int main(void)
{
    const uint8_t *chunks[8];
    size_t sizes[8] = { 0 };
    size_t count = 0;
    unsigned char *file = NULL;
    struct QwDjvuIw44Image *image = NULL;
    unsigned char *rgb = NULL;
    unsigned char *ref = NULL;
    size_t ref_size = 0;
    const unsigned char *ref_pixels;
    size_t ref_pixel_size = 0;
    int ref_width = 0;
    int ref_height = 0;
    size_t ref_channels = 0;
    int width = 0;
    int height = 0;
    int failures = 0;

    if (load_bg44("photo.djvu", chunks, sizes, &count, &file) != 0) {
        fprintf(stderr, "photo: no BG44 chunks\n");
        return 1;
    }
    if (qw_djvu_iw44_decode_chunks(&image, chunks, sizes, count) != 0) {
        fprintf(stderr, "photo: IW44 decode failed\n");
        free(file);
        return 1;
    }
    if (qw_djvu_iw44_size(image, &width, &height) != 0 ||
        width != 64 || height != 48) {
        fprintf(stderr, "photo: size %dx%d\n", width, height);
        ++failures;
    }
    if (!qw_djvu_iw44_is_color(image)) {
        /* The fixture uses the gray profile (300dpi photo encoded as gray);
           the reference PPM stores R=G=B triplets. */
        printf("photo: gray profile (expected)\n");
    }
    rgb = (unsigned char *)malloc((size_t)width * (size_t)height * 3u);
    if (!rgb || qw_djvu_iw44_render_rgb(image, rgb) != 0) {
        fprintf(stderr, "photo: render failed\n");
        ++failures;
    }

    ref = read_file("photo.ref.ppm", &ref_size);
    if (!ref) {
        fprintf(stderr, "photo: reference read failed\n");
        ++failures;
    } else {
        ref_pixels = pnm_pixels(ref, ref_size, 'P', '6', &ref_width,
                                &ref_height, &ref_pixel_size, &ref_channels);
        if (!ref_pixels) {
            fprintf(stderr, "photo: reference parse failed\n");
            ++failures;
        } else if (ref_width != width || ref_height != height ||
                   ref_pixel_size != (size_t)width * (size_t)height * 3u) {
            fprintf(stderr, "photo: reference size mismatch\n");
            ++failures;
        } else if (rgb && memcmp(ref_pixels, rgb, ref_pixel_size) != 0) {
            size_t i;
            fprintf(stderr, "photo: pixel mismatch\n");
            for (i = 0; i < ref_pixel_size; ++i) {
                if (ref_pixels[i] != rgb[i]) {
                    fprintf(stderr, "  first diff at %lu: ref=%u got=%u\n",
                            (unsigned long)i, ref_pixels[i], rgb[i]);
                    break;
                }
            }
            ++failures;
        }
    }

    free(rgb);
    free(ref);
    free(file);
    qw_djvu_iw44_free(image);

    if (failures != 0) {
        fprintf(stderr, "djvu_iw44: %d failure(s)\n", failures);
        return 1;
    }
    printf("djvu_iw44: ok\n");
    return 0;
}

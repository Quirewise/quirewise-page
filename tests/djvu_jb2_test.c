/* Unit tests for the first-party DjVu JB2 bitonal decoder.
 *
 * The decoded page is compared byte-for-byte with bitonal.ref.pgm, which was
 * produced by the previous decoder; this is a real regression oracle. */

#include "djvu_iff.h"
#include "djvu_jb2.h"

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

/* Parse a binary PGM (P5) image. Returns pixel payload pointer and size. */
static const unsigned char *pgm_pixels(const unsigned char *data, size_t size,
                                       int *width, int *height,
                                       size_t *pixel_size)
{
    size_t pos = 0;
    int fields = 0;
    int values[3] = { 0, 0, 0 };

    if (size < 2 || data[0] != 'P' || data[1] != '5') return NULL;
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
    if (fields != 3) return NULL;
    if (pos >= size) return NULL;
    ++pos; /* single whitespace after maxval */
    *width = values[0];
    *height = values[1];
    *pixel_size = (size_t)values[0] * (size_t)values[1];
    if (pos + *pixel_size > size) return NULL;
    return data + pos;
}

int main(void)
{
    unsigned char *file = NULL;
    size_t file_size = 0;
    QwDjvuIffForm form;
    QwDjvuIffChunk sjbz;
    struct QwDjvuJb2Image *image = NULL;
    unsigned char *ref = NULL;
    size_t ref_size = 0;
    const unsigned char *ref_pixels;
    size_t ref_pixel_size = 0;
    int ref_width = 0;
    int ref_height = 0;
    int width = 0;
    int height = 0;
    unsigned char *rendered = NULL;
    int failures = 0;

    file = read_file("bitonal.djvu", &file_size);
    if (!file) { fprintf(stderr, "bitonal: read\n"); return 1; }
    if (qw_djvu_iff_form(file, file_size, &form) != 0 ||
        qw_djvu_iff_find(&form, "Sjbz", &sjbz) != 1) {
        fprintf(stderr, "bitonal: Sjbz not found\n");
        free(file);
        return 1;
    }
    if (qw_djvu_jb2_decode(sjbz.data, sjbz.size, &image) != 0) {
        fprintf(stderr, "bitonal: JB2 decode failed\n");
        free(file);
        return 1;
    }
    if (qw_djvu_jb2_image_size(image, &width, &height) != 0 ||
        width != 64 || height != 48) {
        fprintf(stderr, "bitonal: size %dx%d\n", width, height);
        ++failures;
    }
    rendered = (unsigned char *)malloc((size_t)width * (size_t)height);
    if (!rendered ||
        qw_djvu_jb2_render_gray8(image, rendered, (size_t)width,
                                 (size_t)height) != 0) {
        fprintf(stderr, "bitonal: render failed\n");
        ++failures;
    }

    ref = read_file("bitonal.ref.pgm", &ref_size);
    if (!ref) {
        fprintf(stderr, "bitonal: reference read\n");
        ++failures;
    } else {
        ref_pixels = pgm_pixels(ref, ref_size, &ref_width, &ref_height,
                                &ref_pixel_size);
        if (!ref_pixels) {
            fprintf(stderr, "bitonal: reference parse\n");
            ++failures;
        } else if (ref_width != width || ref_height != height ||
                   ref_pixel_size != (size_t)width * (size_t)height) {
            fprintf(stderr, "bitonal: reference size mismatch\n");
            ++failures;
        } else if (rendered &&
                   memcmp(ref_pixels, rendered, ref_pixel_size) != 0) {
            size_t i;
            fprintf(stderr, "bitonal: pixel mismatch\n");
            for (i = 0; i < ref_pixel_size; ++i) {
                if (ref_pixels[i] != rendered[i]) {
                    fprintf(stderr, "  first diff at %lu: ref=%u got=%u\n",
                            (unsigned long)i, ref_pixels[i], rendered[i]);
                    break;
                }
            }
            ++failures;
        }
    }

    /* Truncated stream must not produce the same page. */
    {
        struct QwDjvuJb2Image *bad = NULL;
        if (sjbz.size > 6 &&
            qw_djvu_jb2_decode(sjbz.data, sjbz.size - 6, &bad) == 0) {
            unsigned char *other = (unsigned char *)malloc((size_t)width * (size_t)height);
            if (other && qw_djvu_jb2_render_gray8(bad, other, (size_t)width,
                                                  (size_t)height) == 0 &&
                rendered &&
                memcmp(other, rendered, (size_t)width * (size_t)height) == 0) {
                fprintf(stderr, "bitonal: truncated stream rendered identically\n");
                ++failures;
            }
            free(other);
            qw_djvu_jb2_free(bad);
        }
    }

    free(rendered);
    free(ref);
    free(file);
    qw_djvu_jb2_free(image);

    if (failures != 0) {
        fprintf(stderr, "djvu_jb2: %d failure(s)\n", failures);
        return 1;
    }
    printf("djvu_jb2: ok\n");
    return 0;
}

#include <quirewise/djvu.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_DJVU_FIXTURES
#define QUIREWISE_DJVU_FIXTURES "tests/data/djvu"
#endif

static unsigned char *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    unsigned char *data;
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return NULL; }
    data = (unsigned char *)malloc((size_t)length);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

static const unsigned char *read_pnm(const unsigned char *data, size_t size,
                                     char expected_kind,
                                     unsigned *width, unsigned *height,
                                     size_t *pixel_offset)
{
    size_t i = 0u;
    unsigned values[3] = {0u, 0u, 0u};
    int value_index = 0;
    if (size < 10u || data[0] != 'P' || data[1] != (unsigned char)expected_kind)
        return NULL;
    i = 2u;
    while (i < size && data[i] != '\n') ++i;
    if (i >= size) return NULL;
    ++i;
    while (i < size && value_index < 3) {
        unsigned value = 0u;
        int digits = 0;
        while (i < size && data[i] == ' ') ++i;
        while (i < size && data[i] >= '0' && data[i] <= '9') {
            value = value * 10u + (unsigned)(data[i] - '0');
            ++i;
            ++digits;
        }
        if (!digits) return NULL;
        values[value_index++] = value;
        while (i < size && (data[i] == ' ' || data[i] == '\r')) ++i;
        if (data[i] == '\n') ++i;
    }
    if (value_index != 3 || values[2] != 255u) return NULL;
    *width = values[0];
    *height = values[1];
    *pixel_offset = i;
    return data + i;
}

static int render_matches(const char *djvu_name, size_t page,
                          const char *pnm_name, unsigned expected_width,
                          unsigned expected_height, QwDjvuPixelFormat expected)
{
    char path[512];
    size_t size = 0u, ref_size = 0u, offset = 0u;
    unsigned char *data;
    unsigned char *ref;
    const unsigned char *ref_pixels;
    unsigned ref_width = 0u, ref_height = 0u;
    QwDjvuImage image;
    int ok;
    snprintf(path, sizeof(path), "%s/%s", QUIREWISE_DJVU_FIXTURES, djvu_name);
    data = read_file(path, &size);
    snprintf(path, sizeof(path), "%s/%s", QUIREWISE_DJVU_FIXTURES, pnm_name);
    ref = read_file(path, &ref_size);
    if (!data || !ref) {
        fprintf(stderr, "cannot read %s/%s\n", djvu_name, pnm_name);
        free(data); free(ref);
        return 0;
    }
    ref_pixels = read_pnm(ref, ref_size,
                          expected == QW_DJVU_PIXEL_RGB24 ? '6' : '5',
                          &ref_width, &ref_height, &offset);
    ok = ref_pixels != NULL && qw_djvu_render_page(data, size, page, 1u,
                                                   &image) == QW_DJVU_OK &&
         image.width == expected_width && image.height == expected_height &&
         image.format == expected && ref_width == expected_width &&
         ref_height == expected_height &&
         image.size == ref_size - offset &&
         memcmp(image.pixels, ref_pixels, image.size) == 0;
    if (!ok) {
        fprintf(stderr, "%s page %zu does not match %s\n", djvu_name, page,
                pnm_name);
    }
    qw_djvu_image_free(&image);
    free(data);
    free(ref);
    return ok;
}

static int test_page_counts(void)
{
    char path[512];
    size_t size = 0u, pages = 0u;
    unsigned char *data;
    int ok = 1;
    snprintf(path, sizeof(path), "%s/photo.djvu", QUIREWISE_DJVU_FIXTURES);
    data = read_file(path, &size);
    if (!data || qw_djvu_get_page_count(data, size, &pages) != QW_DJVU_OK ||
        pages != 1u) ok = 0;
    free(data);
    snprintf(path, sizeof(path), "%s/multipage.djvu", QUIREWISE_DJVU_FIXTURES);
    data = read_file(path, &size);
    if (!data || qw_djvu_get_page_count(data, size, &pages) != QW_DJVU_OK ||
        pages != 2u) ok = 0;
    free(data);
    return ok;
}

static int test_page_info(void)
{
    char path[512];
    size_t size = 0u;
    unsigned char *data;
    QwDjvuPageInfo info;
    int ok;
    snprintf(path, sizeof(path), "%s/bitonal.djvu", QUIREWISE_DJVU_FIXTURES);
    data = read_file(path, &size);
    if (!data) return 0;
    ok = qw_djvu_get_page_info(data, size, 0u, &info) == QW_DJVU_OK &&
         info.width == 64u && info.height == 48u && info.dpi == 300u &&
         info.rotation == 0u;
    free(data);
    return ok;
}

static int test_text(void)
{
    char path[512];
    size_t size = 0u, text_size = 0u;
    unsigned char *data;
    char *text = NULL;
    int ok;
    snprintf(path, sizeof(path), "%s/text.djvu", QUIREWISE_DJVU_FIXTURES);
    data = read_file(path, &size);
    if (!data) return 0;
    ok = qw_djvu_extract_text(data, size, 0u, &text, &text_size) == QW_DJVU_OK &&
         text != NULL && strstr(text, "Hello DjVu") != NULL;
    qw_djvu_text_free(text);
    free(data);
    return ok;
}

static int test_corruption(void)
{
    char path[512];
    size_t size = 0u, pages = 0u;
    unsigned char *data;
    QwDjvuPageInfo info;
    int ok = 1;
    snprintf(path, sizeof(path), "%s/photo.djvu", QUIREWISE_DJVU_FIXTURES);
    data = read_file(path, &size);
    if (!data) return 0;
    /* Truncated container. */
    if (qw_djvu_get_page_count(data, size - 10u, &pages) == QW_DJVU_OK) ok = 0;
    /* Oversized FORM length. */
    {
        unsigned char *copy = (unsigned char *)malloc(size);
        if (!copy) { free(data); return 0; }
        memcpy(copy, data, size);
        copy[8] = 0x7f; copy[9] = 0xff; copy[10] = 0xff; copy[11] = 0xff;
        if (qw_djvu_get_page_count(copy, size, &pages) == QW_DJVU_OK) ok = 0;
        free(copy);
    }
    /* Huge INFO geometry must be refused before allocating a page. */
    {
        unsigned char *copy = (unsigned char *)malloc(size);
        QwDjvuImage image;
        size_t info_offset = 0u;
        if (!copy) { free(data); return 0; }
        memcpy(copy, data, size);
        for (size_t i = 0u; i + 8u <= size; ++i) {
            if (memcmp(copy + i, "INFO", 4u) == 0) { info_offset = i + 8u; break; }
        }
        if (info_offset && info_offset + 4u <= size) {
            copy[info_offset] = 0xff; copy[info_offset + 1u] = 0xff;
            copy[info_offset + 2u] = 0xff; copy[info_offset + 3u] = 0xff;
            if (qw_djvu_get_page_info(copy, size, 0u, &info) != QW_DJVU_OK)
                ok = 0;
            else if (qw_djvu_render_page(copy, size, 0u, 1u, &image) !=
                     QW_DJVU_ERR_RESOURCE_LIMIT)
                ok = 0;
        } else {
            ok = 0;
        }
        free(copy);
    }
    /* Nested FORM chunk inside a page must be rejected. */
    {
        unsigned char *copy = (unsigned char *)malloc(size);
        size_t chunk = 0u;
        if (!copy) { free(data); return 0; }
        memcpy(copy, data, size);
        for (size_t i = 0u; i + 4u <= size; ++i) {
            if (memcmp(copy + i, "Sjbz", 4u) == 0 ||
                memcmp(copy + i, "BG44", 4u) == 0) {
                chunk = i;
                break;
            }
        }
        if (chunk) {
            memcpy(copy + chunk, "FORM", 4u);
            if (qw_djvu_get_page_count(copy, size, &pages) == QW_DJVU_OK)
                ok = 0;
        } else {
            ok = 0;
        }
        free(copy);
    }
    free(data);
    return ok;
}

static int test_corrupt_dirm(void)
{
    char path[512];
    size_t size = 0u, pages = 0u;
    unsigned char *data;
    size_t offset = 0u;
    int ok;
    snprintf(path, sizeof(path), "%s/multipage.djvu", QUIREWISE_DJVU_FIXTURES);
    data = read_file(path, &size);
    if (!data) return 0;
    for (size_t i = 0u; i + 8u <= size; ++i) {
        if (memcmp(data + i, "DIRM", 4u) == 0) { offset = i + 4u; break; }
    }
    if (!offset) { free(data); return 0; }
    data[offset] = 0x7f; data[offset + 1u] = 0xff;
    data[offset + 2u] = 0xff; data[offset + 3u] = 0xff;
    ok = qw_djvu_get_page_count(data, size, &pages) != QW_DJVU_OK;
    free(data);
    return ok;
}

int main(void)
{
    int ok = 1;
    ok &= test_page_counts();
    ok &= test_page_info();
    ok &= render_matches("photo.djvu", 0u, "photo.ref.ppm", 64u, 48u,
                         QW_DJVU_PIXEL_RGB24);
    ok &= render_matches("bitonal.djvu", 0u, "bitonal.ref.pgm", 64u, 48u,
                         QW_DJVU_PIXEL_GRAY8);
    ok &= render_matches("multipage.djvu", 0u, "multi.p1.ref.ppm", 64u, 48u,
                         QW_DJVU_PIXEL_RGB24);
    ok &= render_matches("multipage.djvu", 1u, "multi.p2.ref.pgm", 64u, 48u,
                         QW_DJVU_PIXEL_GRAY8);
    ok &= test_text();
    ok &= test_corruption();
    ok &= test_corrupt_dirm();
    if (!ok) {
        fputs("DjVu decode verification failed\n", stderr);
        return 1;
    }
    puts("DjVu decode matches ddjvu references and rejects malformed input");
    return 0;
}

/* Unit tests for the first-party DjVu IFF reader. */

#include "djvu_iff.h"

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

static int count_chunks(const QwDjvuIffForm *form, char ids[][5], int max,
                        size_t *sizes)
{
    size_t cursor = 0;
    QwDjvuIffChunk chunk;
    int count = 0;
    int rc;

    while ((rc = qw_djvu_iff_next(form, &cursor, &chunk)) == 1) {
        if (count < max) {
            memcpy(ids[count], chunk.id, 5);
            if (sizes) sizes[count] = chunk.size;
        }
        ++count;
    }
    return rc < 0 ? -rc - 1 : count;
}

static int test_bitonal(void)
{
    size_t size = 0;
    unsigned char *data = read_file("bitonal.djvu", &size);
    QwDjvuIffForm form;
    char ids[8][5];
    size_t sizes[8];
    QwDjvuIffChunk info;
    unsigned width, height;
    int count;
    int failures = 0;

    if (!data) return 1;
    if (qw_djvu_iff_form(data, size, &form) != 0 || strcmp(form.type, "DJVU") != 0) {
        fprintf(stderr, "bitonal: form header\n");
        free(data);
        return 1;
    }
    count = count_chunks(&form, ids, 8, sizes);
    if (count != 2 || strcmp(ids[0], "INFO") != 0 || strcmp(ids[1], "Sjbz") != 0) {
        fprintf(stderr, "bitonal: chunk list (%d)\n", count);
        free(data);
        return 1;
    }
    if (sizes[0] != 10u || sizes[1] != 26u) {
        fprintf(stderr, "bitonal: chunk sizes\n");
        ++failures;
    }
    if (qw_djvu_iff_find(&form, "INFO", &info) != 1 || info.size < 4u) {
        fprintf(stderr, "bitonal: INFO lookup\n");
        free(data);
        return 1;
    }
    width = ((unsigned)info.data[0] << 8) | info.data[1];
    height = ((unsigned)info.data[2] << 8) | info.data[3];
    if (width != 64u || height != 48u) {
        fprintf(stderr, "bitonal: INFO dimensions %ux%u\n", width, height);
        ++failures;
    }
    free(data);
    return failures;
}

static int test_photo_and_text(void)
{
    const char *names[2] = { "photo.djvu", "text.djvu" };
    int expected[2] = { 4, 5 };
    int i;
    int failures = 0;

    for (i = 0; i < 2; ++i) {
        size_t size = 0;
        unsigned char *data = read_file(names[i], &size);
        QwDjvuIffForm form;
        char ids[8][5];
        int count;
        int j;

        if (!data) { ++failures; continue; }
        if (qw_djvu_iff_form(data, size, &form) != 0 || strcmp(form.type, "DJVU") != 0) {
            fprintf(stderr, "%s: form header\n", names[i]);
            ++failures;
            free(data);
            continue;
        }
        count = count_chunks(&form, ids, 8, NULL);
        if (count != expected[i]) {
            fprintf(stderr, "%s: chunk count %d != %d\n", names[i], count, expected[i]);
            ++failures;
        }
        for (j = 0; j < count && j < 8; ++j) {
            if (j > 0 && strcmp(ids[j], "BG44") != 0 &&
                !(i == 1 && j == 4 && strcmp(ids[j], "TXTz") == 0)) {
                fprintf(stderr, "%s: unexpected chunk %s at %d\n", names[i], ids[j], j);
                ++failures;
            }
        }
        free(data);
    }
    return failures;
}

static int test_multipage(void)
{
    size_t size = 0;
    unsigned char *data = read_file("multipage.djvu", &size);
    QwDjvuIffForm form;
    size_t cursor = 0;
    QwDjvuIffChunk chunk;
    int forms = 0;
    int dirs = 0;
    int failures = 0;

    if (!data) return 1;
    if (qw_djvu_iff_form(data, size, &form) != 0 || strcmp(form.type, "DJVM") != 0) {
        fprintf(stderr, "multipage: form header\n");
        free(data);
        return 1;
    }
    while (qw_djvu_iff_next(&form, &cursor, &chunk) == 1) {
        if (strcmp(chunk.id, "DIRM") == 0) {
            ++dirs;
        } else if (strcmp(chunk.id, "FORM") == 0) {
            QwDjvuIffForm nested;
            ++forms;
            if (qw_djvu_iff_nested(&chunk, &nested) != 0 ||
                strcmp(nested.type, "DJVU") != 0) {
                fprintf(stderr, "multipage: nested form %d\n", forms);
                ++failures;
            }
        } else {
            fprintf(stderr, "multipage: unexpected chunk %s\n", chunk.id);
            ++failures;
        }
    }
    if (dirs != 1 || forms != 2) {
        fprintf(stderr, "multipage: DIRM=%d FORM=%d\n", dirs, forms);
        ++failures;
    }
    free(data);
    return failures;
}

static int test_malformed(void)
{
    static const unsigned char bad_magic[16] = {
        'N', 'O', 'P', 'E', 0, 0, 0, 8, 'D', 'J', 'V', 'U', 0, 0, 0, 0
    };
    static const unsigned char bad_length[16] = {
        'A', 'T', '&', 'T', 'F', 'O', 'R', 'M', 0xff, 0xff, 0xff, 0xff,
        'D', 'J', 'V', 'U'
    };
    QwDjvuIffForm form;
    int failures = 0;

    if (qw_djvu_iff_form(bad_magic, sizeof(bad_magic), &form) == 0) {
        fprintf(stderr, "malformed: bad magic accepted\n");
        ++failures;
    }
    if (qw_djvu_iff_form(bad_length, sizeof(bad_length), &form) == 0) {
        fprintf(stderr, "malformed: bad length accepted\n");
        ++failures;
    }
    if (qw_djvu_iff_form(NULL, 0, &form) == 0) {
        fprintf(stderr, "malformed: NULL accepted\n");
        ++failures;
    }
    return failures;
}

int main(void)
{
    int failures = 0;

    failures += test_bitonal();
    failures += test_photo_and_text();
    failures += test_multipage();
    failures += test_malformed();

    if (failures != 0) {
        fprintf(stderr, "djvu_iff: %d failure(s)\n", failures);
        return 1;
    }
    printf("djvu_iff: ok\n");
    return 0;
}

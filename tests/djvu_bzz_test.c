/* Unit tests for the first-party DjVu BZZ decoder.
 *
 * The TXTz chunk of text.djvu decompresses to a 66-byte text-zone record whose
 * exact bytes were captured from the previous decoder before this module was
 * introduced; the comparison is therefore a real regression, not a self-check. */

#include "djvu_bzz.h"
#include "djvu_iff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_DJVU_FIXTURES
#define QUIREWISE_DJVU_FIXTURES "tests/data/djvu"
#endif

static const unsigned char k_expected[] = {
    0x00, 0x00, 0x0b, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x44, 0x6a,
    0x56, 0x75, 0x20, 0x01, 0x01, 0x80, 0x00, 0x80, 0x00, 0x80, 0x40,
    0x80, 0x30, 0x80, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x02, 0x06,
    0x80, 0x0a, 0x80, 0x1e, 0x80, 0x22, 0x80, 0x08, 0x80, 0x00, 0x00,
    0x00, 0x06, 0x00, 0x00, 0x00, 0x06, 0x7f, 0xde, 0x80, 0x08, 0x80,
    0x1e, 0x80, 0x06, 0x80, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00
};

static unsigned char *read_file(const char *path, size_t *out_size)
{
    FILE *fh = fopen(path, "rb");
    long size;
    unsigned char *data;
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

static int load_txtz(unsigned char **out_data, size_t *out_size)
{
    char path[1024];
    unsigned char *file;
    size_t file_size = 0;
    QwDjvuIffForm form;
    QwDjvuIffChunk chunk;
    int rc;

    snprintf(path, sizeof(path), "%s/%s", QUIREWISE_DJVU_FIXTURES,
             "text.djvu");
    file = read_file(path, &file_size);
    if (!file) return -1;
    rc = qw_djvu_iff_form(file, file_size, &form);
    if (rc == 0) rc = qw_djvu_iff_find(&form, "TXTz", &chunk);
    if (rc == 1) {
        *out_data = (unsigned char *)malloc(chunk.size);
        if (*out_data) {
            memcpy(*out_data, chunk.data, chunk.size);
            *out_size = chunk.size;
        } else {
            rc = -1;
        }
    } else {
        rc = -1;
    }
    free(file);
    return rc == 1 ? 0 : -1;
}

int main(void)
{
    unsigned char *txtz = NULL;
    size_t txtz_size = 0;
    unsigned char *decoded = NULL;
    size_t decoded_size = 0;
    int failures = 0;
    int rc;

    if (load_txtz(&txtz, &txtz_size) != 0) {
        fprintf(stderr, "could not load TXTz fixture\n");
        return 1;
    }

    rc = qw_djvu_bzz_decode(txtz, txtz_size, &decoded, &decoded_size, 1u << 20);
    if (rc != QW_DJVU_BZZ_OK) {
        fprintf(stderr, "decode failed: %d\n", rc);
        ++failures;
    } else {
        if (decoded_size != sizeof(k_expected)) {
            fprintf(stderr, "size %lu != %lu\n", (unsigned long)decoded_size,
                    (unsigned long)sizeof(k_expected));
            ++failures;
        } else if (memcmp(decoded, k_expected, sizeof(k_expected)) != 0) {
            fprintf(stderr, "payload mismatch\n");
            ++failures;
        }
    }
    free(decoded);
    decoded = NULL;
    decoded_size = 0;

    rc = qw_djvu_bzz_decode(txtz, txtz_size, &decoded, &decoded_size, 32u);
    if (rc != QW_DJVU_BZZ_ERR_LIMIT) {
        fprintf(stderr, "output limit not enforced: %d\n", rc);
        ++failures;
    }
    free(decoded);

    decoded = NULL;
    decoded_size = 0;
    rc = qw_djvu_bzz_decode(txtz, txtz_size - 4u, &decoded, &decoded_size,
                            1u << 20);
    if (rc == QW_DJVU_BZZ_OK && decoded_size == sizeof(k_expected) &&
        memcmp(decoded, k_expected, sizeof(k_expected)) == 0) {
        fprintf(stderr, "truncated stream decoded identically\n");
        ++failures;
    }
    free(decoded);

    if (qw_djvu_bzz_decode(NULL, 0, &decoded, &decoded_size, 1024u) ==
        QW_DJVU_BZZ_OK) {
        fprintf(stderr, "NULL input accepted\n");
        ++failures;
    }

    free(txtz);
    if (failures != 0) {
        fprintf(stderr, "djvu_bzz: %d failure(s)\n", failures);
        return 1;
    }
    printf("djvu_bzz: ok\n");
    return 0;
}

/*
 * Regression: Founder CEB streams that decrypt to garbage must be rejected.
 *
 * The inner PDF declares /Filter /FlateDecode, so after 3DES decryption the
 * payload can be validated as a zlib member. A wrong key (or a container that
 * claims encryption while carrying plaintext) yields random bytes and must
 * surface QW_CEB_ERR_DECODE instead of a "successfully" decrypted broken PDF.
 *
 * The synthetic container is built here, including the Founder outer keystream,
 * so the test needs no binary fixture. The 8-byte CFB64 ciphertext below was
 * produced with the fixed 24-byte key: it decrypts to "notzlib!", whose first
 * byte is an invalid zlib header.
 */

#include <quirewise/ceb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static const uint8_t founder_key[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};

static const uint8_t symmetric_key[24] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
    0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67
};

/* CFB64(cipher) with IV = key[0..7] yields the plaintext "notzlib!". */
static const uint8_t stream_cipher[8] = {
    0x07, 0x78, 0x33, 0x87, 0xe4, 0xdf, 0xb0, 0x09
};

static void founder_keystream(const uint8_t *key, size_t key_size,
                              uint8_t *stream, size_t size)
{
    uint8_t state[256];
    uint8_t i = 0u;
    uint8_t j = 0u;
    size_t index;
    for (index = 0u; index < 256u; ++index) state[index] = (uint8_t)index;
    for (index = 0u; index < 256u; ++index) {
        uint8_t temporary;
        j = (uint8_t)(j + state[index] +
                      (uint8_t)(key[index % key_size] | 0xaau));
        temporary = state[index];
        state[index] = state[j];
        state[j] = temporary;
    }
    j = 0u;
    for (index = 0u; index < size; ++index) {
        uint8_t old_si;
        i = (uint8_t)(i + 1u);
        old_si = state[i];
        j = (uint8_t)(j + old_si);
        state[i] = state[j];
        state[j] = state[old_si];
        stream[index] = state[(uint8_t)(state[i] + state[j])];
    }
}

static void write_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static size_t build_ceb(uint8_t *out, size_t capacity, int with_filter,
                        uint32_t algorithm)
{
    uint8_t pdf[512];
    uint8_t plain[512];
    size_t pdf_size;
    size_t index_start = 0x1fu;
    size_t item_size = 17u;
    size_t position;
    size_t section;
    size_t sections[4];
    size_t sizes[4];
    uint8_t algorithm_bytes[4];
    int written;

    written = snprintf(
        (char *)plain, sizeof(plain),
        "%%PDF-1.4\n"
        "1 0 obj\n"
        "<< /Length 8%s >>\n"
        "stream\n",
        with_filter ? " /Filter /FlateDecode" : "");
    if (written <= 0) return 0u;
    pdf_size = (size_t)written;
    memcpy(plain + pdf_size, stream_cipher, sizeof(stream_cipher));
    pdf_size += sizeof(stream_cipher);
    written = snprintf((char *)plain + pdf_size, sizeof(plain) - pdf_size,
                       "\nendstream\nendobj\n"
                       "trailer\n<< /Encrypt 1 0 R >>\n%%%%EOF\n");
    if (written <= 0) return 0u;
    pdf_size += (size_t)written;

    founder_keystream(founder_key, sizeof(founder_key), pdf, pdf_size);
    for (position = 0u; position < pdf_size; ++position) {
        pdf[position] ^= plain[position];
    }

    write_u32_le(algorithm_bytes, algorithm);
    sections[0] = 0u;
    sizes[0] = pdf_size;
    sections[1] = 1u;
    sizes[1] = sizeof(founder_key);
    sections[2] = 2u;
    sizes[2] = sizeof(symmetric_key);
    sections[3] = 3u;
    sizes[3] = sizeof(algorithm_bytes);

    position = index_start + 4u * item_size;
    if (position >= capacity) return 0u;
    memset(out, 0, position);
    memcpy(out, "Founder CEB", 11u);
    out[0x14] = 4u;
    out[0x15] = 0u;
    for (section = 0u; section < 4u; ++section) {
        size_t item = index_start + section * item_size;
        size_t payload_size = sizes[section];
        if (position + payload_size > capacity) return 0u;
        write_u32_le(out + item, (uint32_t)position);
        write_u32_le(out + item + 4u, (uint32_t)payload_size);
        out[item + 8u] = (uint8_t)section;
        if (section == 0u) {
            memcpy(out + position, pdf, payload_size);
        } else if (section == 1u) {
            memcpy(out + position, founder_key, payload_size);
        } else if (section == 2u) {
            memcpy(out + position, symmetric_key, payload_size);
        } else {
            memcpy(out + position, algorithm_bytes, payload_size);
        }
        position += payload_size;
    }
    return position;
}

int main(void)
{
    static uint8_t container[4096];
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    size_t size;
    QwCebStatus status;

    /* Encrypted-Flate stream + cipher key: decrypted bytes are not zlib. */
    size = build_ceb(container, sizeof(container), 1, 2u);
    CHECK(size > 0u);
    status = qw_ceb_decrypt_to_pdf(container, size, &pdf, &pdf_size);
    CHECK(status == QW_CEB_ERR_DECODE);
    CHECK(pdf == NULL && pdf_size == 0u);

    /* Same container, algorithm 0: streams are plaintext, so it must open. */
    size = build_ceb(container, sizeof(container), 1, 0u);
    CHECK(size > 0u);
    status = qw_ceb_decrypt_to_pdf(container, size, &pdf, &pdf_size);
    CHECK(status == QW_CEB_OK && pdf && pdf_size > 5u);
    CHECK(memcmp(pdf, "%PDF-", 5u) == 0);
    qw_ceb_pdf_free(pdf);
    pdf = NULL;

    /* Encrypted stream without a filter cannot be probed: still accepted. */
    size = build_ceb(container, sizeof(container), 0, 2u);
    CHECK(size > 0u);
    status = qw_ceb_decrypt_to_pdf(container, size, &pdf, &pdf_size);
    CHECK(status == QW_CEB_OK && pdf && pdf_size > 5u);
    CHECK(memcmp(pdf, "%PDF-", 5u) == 0);
    qw_ceb_pdf_free(pdf);
    return 0;
}

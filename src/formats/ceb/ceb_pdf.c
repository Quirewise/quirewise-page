#include "ceb_internal.h"

#include <quirewise/compression.h>
#include <quirewise/crypto.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CEB_ALGORITHM_NONE = 0,
    CEB_ALGORITHM_3DES_OFB = 1,
    CEB_ALGORITHM_3DES_CFB64 = 2,
    CEB_STREAM_REKEY_BYTES = 256,
    CEB_RSA_BYTES = 64,
    CEB_3DES_KEY_BYTES = 24
};

typedef struct CebIntegerObject {
    uint64_t object_number;
    uint64_t generation;
    uint64_t value;
} CebIntegerObject;

typedef struct CebIntegerMap {
    CebIntegerObject *items;
    size_t count;
    size_t capacity;
} CebIntegerMap;

static const char ceb_rsa_modulus_hex[] =
    "91437cd83d0722ce410bd96ca80cff34895a315e25128bc32529d5f614e45097"
    "e12e450c68daf1ad8d8e740ab708564f4f317b8012c44856de567d58522497db";
static const char ceb_rsa_exponent_hex[] =
    "f384ff17a8165a0eceb0a526f54412a99a88ec6968b5e14faf7a08bee92ffde3"
    "5b18c54697d86ecc639700e642bc91757e522dc5ef4c95ccd746d2d259db00c5";

static int pdf_is_space(uint8_t value)
{
    return value == 0u || value == 9u || value == 10u || value == 12u ||
           value == 13u || value == 32u;
}

static int pdf_is_delimiter(uint8_t value)
{
    return pdf_is_space(value) || value == '(' || value == ')' ||
           value == '<' || value == '>' || value == '[' || value == ']' ||
           value == '{' || value == '}' || value == '/' || value == '%';
}

static int pdf_token_at(const uint8_t *data,
                        size_t size,
                        size_t offset,
                        const char *token,
                        size_t token_size)
{
    if (!data || !token || offset > size || token_size > size - offset ||
        memcmp(data + offset, token, token_size) != 0) {
        return 0;
    }
    if (offset > 0u && !pdf_is_delimiter(data[offset - 1u])) return 0;
    if (offset + token_size < size &&
        !pdf_is_delimiter(data[offset + token_size])) return 0;
    return 1;
}

static int pdf_find_token(const uint8_t *data,
                          size_t size,
                          size_t start,
                          const char *token,
                          size_t token_size,
                          size_t *found)
{
    size_t offset;
    if (!data || !token || !found || start > size || token_size > size) return 0;
    for (offset = start; offset <= size - token_size; ++offset) {
        if (pdf_token_at(data, size, offset, token, token_size)) {
            *found = offset;
            return 1;
        }
    }
    return 0;
}

static int pdf_find_last_token(const uint8_t *data,
                               size_t size,
                               size_t before,
                               const char *token,
                               size_t token_size,
                               size_t *found)
{
    size_t offset;
    int have_match = 0;
    if (!data || !token || !found || before > size) return 0;
    if (token_size > before) return 0;
    for (offset = 0; offset <= before - token_size; ++offset) {
        if (pdf_token_at(data, size, offset, token, token_size)) {
            *found = offset;
            have_match = 1;
        }
    }
    return have_match;
}

static size_t pdf_skip_space(const uint8_t *data, size_t size, size_t offset)
{
    while (offset < size && pdf_is_space(data[offset])) ++offset;
    return offset;
}

static int pdf_read_u64(const uint8_t *data,
                        size_t size,
                        size_t *cursor,
                        uint64_t *value)
{
    size_t offset;
    uint64_t parsed = 0;
    int digits = 0;
    if (!data || !cursor || !value || *cursor > size) return 0;
    offset = pdf_skip_space(data, size, *cursor);
    while (offset < size && data[offset] >= '0' && data[offset] <= '9') {
        uint64_t digit = (uint64_t)(data[offset] - '0');
        if (parsed > (UINT64_MAX - digit) / 10u) return 0;
        parsed = parsed * 10u + digit;
        ++offset;
        digits = 1;
    }
    if (!digits) return 0;
    *cursor = offset;
    *value = parsed;
    return 1;
}

static int pdf_read_u64_backward(const uint8_t *data,
                                 size_t *cursor,
                                 uint64_t *value)
{
    size_t end;
    size_t start;
    uint64_t parsed = 0;
    uint64_t place = 1;
    if (!data || !cursor || !value) return 0;
    while (*cursor > 0u && pdf_is_space(data[*cursor - 1u])) --*cursor;
    end = *cursor;
    while (*cursor > 0u && data[*cursor - 1u] >= '0' &&
           data[*cursor - 1u] <= '9') {
        uint64_t digit = (uint64_t)(data[*cursor - 1u] - '0');
        if (digit && place > UINT64_MAX / digit) return 0;
        if (parsed > UINT64_MAX - digit * place) return 0;
        parsed += digit * place;
        if (place <= UINT64_MAX / 10u) place *= 10u;
        --*cursor;
    }
    start = *cursor;
    if (start == end) return 0;
    *value = parsed;
    return 1;
}

static int ceb_hex_to_bytes(const char *hex, uint8_t *out, size_t out_size)
{
    size_t i;
    if (!hex || !out || strlen(hex) != out_size * 2u) return 0;
    for (i = 0; i < out_size; ++i) {
        unsigned high;
        unsigned low;
        char a = hex[i * 2u];
        char b = hex[i * 2u + 1u];
        if (a >= '0' && a <= '9') high = (unsigned)(a - '0');
        else if (a >= 'a' && a <= 'f') high = (unsigned)(a - 'a' + 10);
        else if (a >= 'A' && a <= 'F') high = (unsigned)(a - 'A' + 10);
        else return 0;
        if (b >= '0' && b <= '9') low = (unsigned)(b - '0');
        else if (b >= 'a' && b <= 'f') low = (unsigned)(b - 'a' + 10);
        else if (b >= 'A' && b <= 'F') low = (unsigned)(b - 'A' + 10);
        else return 0;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return 1;
}

static int ceb_unwrap_rsa_key(const uint8_t wrapped[CEB_RSA_BYTES],
                              uint8_t key[CEB_3DES_KEY_BYTES])
{
    uint8_t modulus_le[CEB_RSA_BYTES] = {0};
    uint8_t exponent_le[CEB_RSA_BYTES] = {0};
    uint8_t clear[CEB_RSA_BYTES] = {0};
    uint32_t key_size = 0;
    int ok = 0;

    if (key) qw_crypto_secure_zero(key, CEB_3DES_KEY_BYTES);
    if (!wrapped || !key ||
        !ceb_hex_to_bytes(ceb_rsa_modulus_hex, modulus_le, sizeof(modulus_le)) ||
        !ceb_hex_to_bytes(ceb_rsa_exponent_hex, exponent_le, sizeof(exponent_le))) {
        goto done;
    }
    if (qw_crypto_rsa_mod_exp_512_le(wrapped, exponent_le, modulus_le,
                                     clear) != QW_CRYPTO_OK) {
        goto done;
    }
    key_size = (uint32_t)clear[0] | ((uint32_t)clear[1] << 8) |
               ((uint32_t)clear[2] << 16) | ((uint32_t)clear[3] << 24);
    if (key_size != CEB_3DES_KEY_BYTES) goto done;
    memcpy(key, clear + 4u, CEB_3DES_KEY_BYTES);
    ok = 1;

done:
    if (!ok && key) qw_crypto_secure_zero(key, CEB_3DES_KEY_BYTES);
    qw_crypto_secure_zero(modulus_le, sizeof(modulus_le));
    qw_crypto_secure_zero(exponent_le, sizeof(exponent_le));
    qw_crypto_secure_zero(clear, sizeof(clear));
    qw_crypto_secure_zero(&key_size, sizeof(key_size));
    return ok;
}

static int ceb_resolve_3des_key(const uint8_t *key_blob,
                                size_t key_blob_size,
                                uint32_t algorithm,
                                uint8_t key[CEB_3DES_KEY_BYTES])
{
    if (key) qw_crypto_secure_zero(key, CEB_3DES_KEY_BYTES);
    if (!key_blob || !key || key_blob_size == 0u) return 0;
    if ((algorithm & 0x80000000u) != 0u) {
        if (key_blob_size != CEB_RSA_BYTES) return 0;
        return ceb_unwrap_rsa_key(key_blob, key);
    }
    if (key_blob_size != CEB_3DES_KEY_BYTES) return 0;
    memcpy(key, key_blob, CEB_3DES_KEY_BYTES);
    return 1;
}

static void ceb_integer_map_free(CebIntegerMap *map)
{
    if (!map) return;
    free(map->items);
    memset(map, 0, sizeof(*map));
}

static int ceb_integer_map_add(CebIntegerMap *map,
                               uint64_t object_number,
                               uint64_t generation,
                               uint64_t value)
{
    CebIntegerObject *items;
    size_t capacity;
    if (!map) return 0;
    if (map->count == map->capacity) {
        capacity = map->capacity ? map->capacity * 2u : 32u;
        if (capacity < map->capacity || capacity > SIZE_MAX / sizeof(*items)) return 0;
        items = (CebIntegerObject *)realloc(map->items,
                                            capacity * sizeof(*items));
        if (!items) return 0;
        map->items = items;
        map->capacity = capacity;
    }
    map->items[map->count].object_number = object_number;
    map->items[map->count].generation = generation;
    map->items[map->count].value = value;
    ++map->count;
    return 1;
}

static int ceb_pdf_object_header(const uint8_t *data,
                                 size_t size,
                                 size_t object_keyword,
                                 uint64_t *object_number,
                                 uint64_t *generation)
{
    size_t cursor = object_keyword;
    if (!pdf_token_at(data, size, object_keyword, "obj", 3u) ||
        !pdf_read_u64_backward(data, &cursor, generation) ||
        !pdf_read_u64_backward(data, &cursor, object_number)) {
        return 0;
    }
    if (cursor > 0u && !pdf_is_delimiter(data[cursor - 1u])) return 0;
    return 1;
}

static int ceb_pdf_collect_integer_objects(const uint8_t *pdf,
                                           size_t pdf_size,
                                           CebIntegerMap *map)
{
    size_t position = 0;
    while (pdf_find_token(pdf, pdf_size, position, "obj", 3u, &position)) {
        uint64_t object_number;
        uint64_t generation;
        uint64_t value;
        size_t cursor;
        size_t endobj;
        if (ceb_pdf_object_header(pdf, pdf_size, position,
                                  &object_number, &generation)) {
            cursor = position + 3u;
            if (pdf_read_u64(pdf, pdf_size, &cursor, &value)) {
                cursor = pdf_skip_space(pdf, pdf_size, cursor);
                if (pdf_token_at(pdf, pdf_size, cursor, "endobj", 6u)) {
                    if (!ceb_integer_map_add(map, object_number,
                                             generation, value)) {
                        return QW_PAGE_ERR_OUT_OF_MEMORY;
                    }
                } else if (pdf_find_token(pdf, pdf_size, cursor,
                                          "endobj", 6u, &endobj)) {
                    (void)endobj;
                }
            }
        }
        position += 3u;
        if (position >= pdf_size) break;
    }
    return QW_PAGE_OK;
}

static int ceb_integer_map_lookup(const CebIntegerMap *map,
                                  uint64_t object_number,
                                  uint64_t generation,
                                  uint64_t *value)
{
    size_t i;
    if (!map || !value) return 0;
    for (i = 0; i < map->count; ++i) {
        if (map->items[i].object_number == object_number &&
            map->items[i].generation == generation) {
            *value = map->items[i].value;
            return 1;
        }
    }
    return 0;
}

static int ceb_pdf_resolve_length(const uint8_t *pdf,
                                  size_t pdf_size,
                                  const CebIntegerMap *map,
                                  size_t object_start,
                                  size_t stream_keyword,
                                  uint64_t *length)
{
    size_t scan = object_start;
    size_t length_keyword = 0;
    int found = 0;
    if (object_start > stream_keyword || stream_keyword > pdf_size) return 0;
    while (pdf_find_token(pdf, stream_keyword, scan, "/Length", 7u,
                          &length_keyword)) {
        found = 1;
        scan = length_keyword + 7u;
    }
    if (!found) return 0;
    {
        size_t cursor = length_keyword + 7u;
        size_t after_first;
        uint64_t first;
        uint64_t second;
        if (!pdf_read_u64(pdf, stream_keyword, &cursor, &first)) return 0;
        after_first = pdf_skip_space(pdf, stream_keyword, cursor);
        cursor = after_first;
        if (pdf_read_u64(pdf, stream_keyword, &cursor, &second)) {
            cursor = pdf_skip_space(pdf, stream_keyword, cursor);
            if (pdf_token_at(pdf, stream_keyword, cursor, "R", 1u)) {
                return ceb_integer_map_lookup(map, first, second, length);
            }
        }
        *length = first;
        return 1;
    }
}

static int ceb_pdf_stream_extent(const uint8_t *pdf,
                                 size_t pdf_size,
                                 const CebIntegerMap *map,
                                 size_t object_start,
                                 size_t stream_keyword,
                                 size_t *data_start,
                                 size_t *data_size,
                                 size_t *endstream_end)
{
    size_t start = stream_keyword + 6u;
    size_t endstream;
    uint64_t declared_length;
    if (start >= pdf_size) return 0;
    if (pdf[start] == '\r') {
        ++start;
        if (start < pdf_size && pdf[start] == '\n') ++start;
    } else if (pdf[start] == '\n') {
        ++start;
    } else {
        return 0;
    }
    if (ceb_pdf_resolve_length(pdf, pdf_size, map, object_start,
                               stream_keyword, &declared_length) &&
        declared_length <= (uint64_t)(pdf_size - start)) {
        size_t after_data = start + (size_t)declared_length;
        size_t marker = after_data;
        if (marker < pdf_size && pdf[marker] == '\r') ++marker;
        if (marker < pdf_size && pdf[marker] == '\n') ++marker;
        if (pdf_token_at(pdf, pdf_size, marker, "endstream", 9u)) {
            *data_start = start;
            *data_size = (size_t)declared_length;
            *endstream_end = marker + 9u;
            return 1;
        }
    }
    if (!pdf_find_token(pdf, pdf_size, start, "endstream", 9u, &endstream) ||
        endstream < start) {
        return 0;
    }
    *data_size = endstream - start;
    if (*data_size > 0u && pdf[start + *data_size - 1u] == '\n') {
        --*data_size;
        if (*data_size > 0u && pdf[start + *data_size - 1u] == '\r') {
            --*data_size;
        }
    } else if (*data_size > 0u && pdf[start + *data_size - 1u] == '\r') {
        --*data_size;
    }
    *data_start = start;
    *endstream_end = endstream + 9u;
    return 1;
}

static int ceb_pdf_crypt_stream(uint8_t *bytes,
                                size_t size,
                                const uint8_t key[CEB_3DES_KEY_BYTES],
                                uint32_t algorithm)
{
    uint8_t iv[8] = {0};
    size_t offset;
    int rc = QW_PAGE_OK;
    if (!key || (size != 0u && !bytes)) return QW_PAGE_ERR_INVALID_ARGUMENT;
    for (offset = 0; offset < size;) {
        size_t chunk = size - offset;
        QwCryptoStatus crypto_status;
        if (chunk > CEB_STREAM_REKEY_BYTES) chunk = CEB_STREAM_REKEY_BYTES;
        memcpy(iv, key, sizeof(iv));
        if ((algorithm & 0x7fffffffu) == CEB_ALGORITHM_3DES_CFB64) {
            crypto_status = qw_crypto_3des_cfb64_decrypt(
                key, iv, bytes + offset, chunk);
        } else {
            crypto_status = qw_crypto_3des_ofb64_crypt(
                key, iv, bytes + offset, chunk);
        }
        qw_crypto_secure_zero(iv, sizeof(iv));
        if (crypto_status != QW_CRYPTO_OK) {
            rc = QW_PAGE_ERR_DECODE;
            break;
        }
        offset += chunk;
    }
    qw_crypto_secure_zero(iv, sizeof(iv));
    return rc;
}

/* True when the object dictionary in [start, end) declares a Flate filter.
 * PDF admits the /Fl abbreviation as well as the full name. */
static int ceb_dict_has_flate(const uint8_t *pdf, size_t start, size_t end)
{
    size_t position = start;
    while (position + 2u < end) {
        if (pdf[position] == '/') {
            if (position + 12u <= end &&
                memcmp(pdf + position, "/FlateDecode", 12u) == 0) {
                return 1;
            }
            if (position + 3u <= end && memcmp(pdf + position, "/Fl", 3u) == 0 &&
                (position + 3u == end || pdf[position + 3u] <= 32u)) {
                return 1;
            }
        }
        ++position;
    }
    return 0;
}

static int ceb_pdf_decrypt_streams(uint8_t *pdf,
                                   size_t pdf_size,
                                   const uint8_t key[CEB_3DES_KEY_BYTES],
                                   uint32_t algorithm,
                                   size_t *stream_count)
{
    CebIntegerMap length_map = {0};
    size_t scan = 0;
    size_t object_start = 0;
    size_t count = 0;
    int rc;
    rc = ceb_pdf_collect_integer_objects(pdf, pdf_size, &length_map);
    if (rc != QW_PAGE_OK) {
        ceb_integer_map_free(&length_map);
        return rc;
    }
    while (scan < pdf_size) {
        size_t keyword;
        size_t data_start;
        size_t data_size;
        size_t endstream_end;
        if (!pdf_find_token(pdf, pdf_size, scan, "stream", 6u, &keyword)) break;
        if (!ceb_pdf_stream_extent(pdf, pdf_size, &length_map,
                                   object_start, keyword, &data_start,
                                   &data_size, &endstream_end)) {
            ceb_integer_map_free(&length_map);
            return QW_PAGE_ERR_DECODE;
        }
        rc = ceb_pdf_crypt_stream(pdf + data_start, data_size, key, algorithm);
        if (rc != QW_PAGE_OK) {
            ceb_integer_map_free(&length_map);
            return rc;
        }
        /* A wrong key produces random bytes but leaves the file structure
         * intact, so the outer call would report success and hand a broken
         * PDF to the caller. Flate streams give a cheap integrity signal:
         * decrypting with the right key yields a valid zlib member. */
        if (data_size && ceb_dict_has_flate(pdf, object_start, keyword)) {
            uint8_t probe[64];
            size_t produced = 0u;
            QwCompressionInflateStatus status =
                qw_compression_inflate_zlib_bounded(
                    pdf + data_start, data_size, probe, sizeof(probe),
                    &produced);
            if (status == QW_COMPRESSION_INFLATE_ERROR) {
                ceb_integer_map_free(&length_map);
                return QW_PAGE_ERR_DECODE;
            }
        }
        ++count;
        scan = endstream_end;
        object_start = scan;
    }
    ceb_integer_map_free(&length_map);
    if (stream_count) *stream_count = count;
    return count ? QW_PAGE_OK : QW_PAGE_ERR_DECODE;
}

static int ceb_pdf_blank_encrypt_reference(uint8_t *pdf,
                                           size_t pdf_size,
                                           int *removed,
                                           int *malformed)
{
    size_t region_start = 0;
    size_t trailer;
    size_t marker;
    size_t encrypt;
    int have_region = 0;
    if (removed) *removed = 0;
    if (malformed) *malformed = 0;

    if (pdf_find_last_token(pdf, pdf_size, pdf_size,
                            "startxref", 9u, &marker)) {
        size_t cursor = pdf_skip_space(pdf, pdf_size, marker + 9u);
        uint64_t xref_offset;
        if (pdf_read_u64(pdf, pdf_size, &cursor, &xref_offset) &&
            xref_offset < pdf_size) {
            region_start = (size_t)xref_offset;
            have_region = 1;
        }
    }
    if (!have_region &&
        pdf_find_last_token(pdf, pdf_size, pdf_size, "trailer", 7u, &trailer)) {
        region_start = trailer + 7u;
        have_region = 1;
    }
    if (!have_region ||
        !pdf_find_token(pdf, pdf_size, region_start, "/Encrypt", 8u, &encrypt)) {
        return QW_PAGE_OK;
    }
    {
        size_t cursor = pdf_skip_space(pdf, pdf_size, encrypt + 8u);
        uint64_t object_number;
        uint64_t generation;
        if (!pdf_read_u64(pdf, pdf_size, &cursor, &object_number) ||
            !pdf_read_u64(pdf, pdf_size, &cursor, &generation)) {
            if (malformed) *malformed = 1;
            return QW_PAGE_ERR_FORMAT;
        }
        cursor = pdf_skip_space(pdf, pdf_size, cursor);
        if (!pdf_token_at(pdf, pdf_size, cursor, "R", 1u)) {
            (void)object_number;
            (void)generation;
            if (malformed) *malformed = 1;
            return QW_PAGE_ERR_FORMAT;
        }
        memset(pdf + encrypt, ' ', cursor + 1u - encrypt);
        if (removed) *removed = 1;
    }
    return QW_PAGE_OK;
}

int qw_ceb_decrypt_internal_pdf(uint8_t *pdf,
                                size_t pdf_size,
                                const uint8_t *key_blob,
                                size_t key_blob_size,
                                int has_algorithm,
                                uint32_t algorithm,
                                QwConvertReport *report)
{
    uint8_t key[CEB_3DES_KEY_BYTES] = {0};
    uint32_t cipher;
    size_t streams = 0;
    int removed = 0;
    int malformed = 0;
    int rc;
    if (!pdf || pdf_size < 5u || memcmp(pdf, "%PDF-", 5u) != 0) {
        rc = QW_PAGE_ERR_FORMAT;
        goto done;
    }
    if (!has_algorithm) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "legacy CEB body decrypted to PDF; no inner cipher metadata");
        rc = QW_PAGE_OK;
        goto done;
    }
    cipher = algorithm & 0x7fffffffu;
    if (cipher == CEB_ALGORITHM_NONE) {
        rc = ceb_pdf_blank_encrypt_reference(pdf, pdf_size, &removed, &malformed);
        if (rc != QW_PAGE_OK) {
            if (report) snprintf(report->note, sizeof(report->note),
                                 malformed ? "CEB PDF /Encrypt reference is malformed"
                                           : "CEB PDF /Encrypt reference could not be removed");
            goto done;
        }
        if (report) snprintf(report->note, sizeof(report->note),
                             "CEB outer body decrypted; algorithm ID marks PDF streams as unencrypted; %s",
                             removed ? "/Encrypt reference blanked"
                                     : "no /Encrypt reference found");
        rc = QW_PAGE_OK;
        goto done;
    }
    if (cipher != CEB_ALGORITHM_3DES_OFB &&
        cipher != CEB_ALGORITHM_3DES_CFB64) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "unsupported CEB stream cipher ID %u", cipher);
        rc = QW_PAGE_ERR_UNSUPPORTED;
        goto done;
    }
    if (!ceb_resolve_3des_key(key_blob, key_blob_size, algorithm, key)) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "CEB 3DES key is malformed or RSA unwrap failed");
        rc = QW_PAGE_ERR_DECODE;
        goto done;
    }
    rc = ceb_pdf_decrypt_streams(pdf, pdf_size, key, algorithm, &streams);
    if (rc != QW_PAGE_OK) {
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "CEB PDF stream boundary scan failed");
        }
        goto done;
    }
    rc = ceb_pdf_blank_encrypt_reference(pdf, pdf_size, &removed, &malformed);
    if (rc != QW_PAGE_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             malformed ? "CEB PDF /Encrypt reference is malformed"
                                       : "CEB PDF /Encrypt reference could not be removed");
        goto done;
    }
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "CEB outer body and %zu 3DES PDF streams decrypted; %s",
                 streams, removed ? "/Encrypt reference blanked"
                                  : "no /Encrypt reference found");
    }
    rc = QW_PAGE_OK;

done:
    qw_crypto_secure_zero(key, sizeof(key));
    return rc;
}

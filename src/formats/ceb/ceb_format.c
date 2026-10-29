#if !defined(QUIREWISE_STANDALONE_CEB)
#include "../../core/format_registry.h"
#endif
#include "../../core/byte_utils.h"

#include "ceb_internal.h"
#include "quirewise/ceb.h"
#include <quirewise/crypto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CEB_MAGIC_SIZE = 11,
    CEB_HEADER_SIZE = 0x16,
    CEB_LEGACY_INDEX_START = 0x16,
    CEB_MODERN_INDEX_START = 0x1f,
    CEB_INDEX_ITEM_SIZE = 17,
    CEB_MAX_SECTIONS = 64,
    CEB_CIPHER_BLOCK_SIZE = 0x10000
};

typedef struct CebEntry {
    size_t offset;
    size_t size;
    uint8_t type;
    int found;
} CebEntry;

typedef struct CebLayout {
    CebEntry pdf;
    CebEntry rc4_key;
    CebEntry symmetric_key;
    CebEntry algorithm;
    uint32_t algorithm_id;
    int has_algorithm;
    int modern;
} CebLayout;



static int ceb_probe(const void *data, size_t size)
{
    return data && size >= CEB_MAGIC_SIZE &&
           memcmp(data, "Founder CEB", CEB_MAGIC_SIZE) == 0;
}

static int validate_entry(const CebEntry *entry, size_t file_size)
{
    return entry && entry->found && entry->offset <= file_size &&
           entry->size <= file_size - entry->offset;
}

static void founder_keystream(const uint8_t *key,
                              size_t key_size,
                              uint8_t *stream,
                              size_t stream_size)
{
    uint8_t state[256] = {0};
    uint8_t i = 0;
    uint8_t j = 0;
    size_t n;
    if (!stream || stream_size == 0u) goto done;
    memset(stream, 0, stream_size);
    if (!key || key_size == 0u) goto done;
    for (n = 0; n < sizeof(state); ++n) state[n] = (uint8_t)n;
    for (n = 0; n < sizeof(state); ++n) {
        j = (uint8_t)(j + state[n] + (uint8_t)(key[n % key_size] | 0xaau));
        {
            uint8_t temporary = state[n];
            state[n] = state[j];
            state[j] = temporary;
        }
    }
    for (n = 0; n < stream_size; ++n) {
        uint8_t old_si;
        uint8_t index;
        i = (uint8_t)(i + 1u);
        old_si = state[i];
        j = (uint8_t)(j + old_si);
        state[i] = state[j];
        state[j] = state[old_si];
        index = (uint8_t)(state[old_si] + state[i]);
        stream[n] = state[index];
    }
done:
    qw_crypto_secure_zero(state, sizeof(state));
    qw_crypto_secure_zero(&i, sizeof(i));
    qw_crypto_secure_zero(&j, sizeof(j));
}

static void set_note(QwConvertReport *report, const char *message)
{
    if (report && message) {
        snprintf(report->note, sizeof(report->note), "%s", message);
    }
}

static int ceb_layout_has_signature(const uint8_t *bytes,
                                    size_t size,
                                    const CebLayout *layout)
{
    uint8_t stream[5] = {0};
    static const uint8_t signature[5] = {'%', 'P', 'D', 'F', '-'};
    size_t i;
    int matches = 1;
    if (!bytes || !layout || !validate_entry(&layout->pdf, size) ||
        !validate_entry(&layout->rc4_key, size) || layout->pdf.size < 5u ||
        layout->rc4_key.size == 0u) {
        return 0;
    }
    founder_keystream(bytes + layout->rc4_key.offset,
                      layout->rc4_key.size, stream, sizeof(stream));
    for (i = 0; i < sizeof(signature); ++i) {
        if ((uint8_t)(bytes[layout->pdf.offset + i] ^ stream[i]) != signature[i]) {
            matches = 0;
            break;
        }
    }
    qw_crypto_secure_zero(stream, sizeof(stream));
    return matches;
}

static int ceb_parse_modern_layout(const uint8_t *bytes,
                                   size_t size,
                                   CebLayout *layout)
{
    uint16_t count;
    size_t index_end;
    size_t i;
    CebEntry sections[CEB_MAX_SECTIONS];
    uint32_t fallback_algorithm = 0;
    int have_fallback_algorithm = 0;
    if (!bytes || !layout || size < CEB_HEADER_SIZE) return 0;
    count = qw_read_u16_le(bytes + 0x14u);
    if (count == 0u || count >= CEB_MAX_SECTIONS) {
        return 0;
    }
    index_end = CEB_MODERN_INDEX_START + (size_t)count * CEB_INDEX_ITEM_SIZE;
    if (index_end > size) return 0;
    memset(sections, 0, sizeof(sections));
    memset(layout, 0, sizeof(*layout));

    for (i = 0; i < count; ++i) {
        const uint8_t *item = bytes + CEB_MODERN_INDEX_START +
                              i * CEB_INDEX_ITEM_SIZE;
        CebEntry entry;
        entry.offset = (size_t)qw_read_u32_le(item);
        entry.size = (size_t)qw_read_u32_le(item + 4u);
        entry.type = item[8];
        entry.found = entry.size > 0u && entry.offset <= size &&
                      entry.size <= size - entry.offset;
        sections[i] = entry;
        if (!entry.found) continue;
        if (!layout->pdf.found || entry.size > layout->pdf.size) {
            layout->pdf = entry;
        }
        if (entry.size == 16u && !layout->rc4_key.found) {
            layout->rc4_key = entry;
        }
        if ((entry.size == 24u || entry.size == 64u) &&
            !layout->symmetric_key.found) {
            layout->symmetric_key = entry;
        }
        if (entry.size == 4u) {
            uint32_t candidate = qw_read_u32_le(bytes + entry.offset);
            uint32_t cipher = candidate & 0x7fffffffu;
            if (!have_fallback_algorithm) {
                fallback_algorithm = candidate;
                have_fallback_algorithm = 1;
            }
            if (cipher <= 2u) {
                layout->algorithm = entry;
                layout->algorithm_id = candidate;
                layout->has_algorithm = 1;
            }
        }
    }
    if (!layout->has_algorithm && have_fallback_algorithm) {
        for (i = 0; i < count; ++i) {
            if (sections[i].found && sections[i].size == 4u &&
                qw_read_u32_le(bytes + sections[i].offset) == fallback_algorithm) {
                layout->algorithm = sections[i];
                layout->algorithm_id = fallback_algorithm;
                layout->has_algorithm = 1;
                break;
            }
        }
    }
    layout->modern = 1;
    return layout->pdf.found && layout->rc4_key.found &&
           (!layout->symmetric_key.found || layout->has_algorithm);
}

static int ceb_parse_legacy_layout(const uint8_t *bytes,
                                   size_t size,
                                   CebLayout *layout)
{
    uint16_t count;
    size_t index_end;
    size_t i;
    if (!bytes || !layout || size < CEB_HEADER_SIZE) return 0;
    count = qw_read_u16_le(bytes + 0x14u);
    if (count == 0u || count >= CEB_MAX_SECTIONS) {
        return 0;
    }
    index_end = CEB_LEGACY_INDEX_START + (size_t)count * CEB_INDEX_ITEM_SIZE;
    if (index_end > size) return 0;
    memset(layout, 0, sizeof(*layout));
    for (i = 0; i < count; ++i) {
        const uint8_t *item = bytes + CEB_LEGACY_INDEX_START +
                              i * CEB_INDEX_ITEM_SIZE;
        CebEntry entry;
        entry.type = item[0];
        entry.offset = (size_t)qw_read_u32_le(item + 9u);
        entry.size = (size_t)qw_read_u32_le(item + 13u);
        entry.found = entry.size > 0u && validate_entry(&entry, size);
        if (!entry.found) continue;
        if (entry.type == 1u) layout->pdf = entry;
        else if (entry.type == 3u) layout->rc4_key = entry;
    }
    layout->modern = 0;
    return layout->pdf.found && layout->rc4_key.found &&
           layout->rc4_key.size > 0u;
}

static int ceb_decrypt_to_pdf(const uint8_t *bytes,
                              size_t size,
                              const CebLayout *layout,
                              uint8_t **out_pdf,
                              size_t *out_size,
                              QwConvertReport *report)
{
    uint8_t stream[CEB_CIPHER_BLOCK_SIZE] = {0};
    uint8_t *output;
    size_t offset;
    int rc;
    if (!bytes || !layout || !out_pdf || !out_size ||
        !validate_entry(&layout->pdf, size) ||
        !validate_entry(&layout->rc4_key, size) ||
        layout->rc4_key.size == 0u || layout->pdf.size < 5u) {
        return QW_PAGE_ERR_FORMAT;
    }
    output = (uint8_t *)malloc(layout->pdf.size);
    if (!output) return QW_PAGE_ERR_OUT_OF_MEMORY;
    founder_keystream(bytes + layout->rc4_key.offset,
                      layout->rc4_key.size, stream, sizeof(stream));
    for (offset = 0; offset < layout->pdf.size;) {
        size_t chunk = layout->pdf.size - offset;
        size_t i;
        if (chunk > CEB_CIPHER_BLOCK_SIZE) chunk = CEB_CIPHER_BLOCK_SIZE;
        for (i = 0; i < chunk; ++i) {
            output[offset + i] =
                bytes[layout->pdf.offset + offset + i] ^ stream[i];
        }
        offset += chunk;
    }
    if (memcmp(output, "%PDF-", 5u) != 0) {
        qw_crypto_secure_zero(output, layout->pdf.size);
        free(output);
        qw_crypto_secure_zero(stream, sizeof(stream));
        set_note(report, "Founder CEB outer body did not decrypt to a PDF");
        return QW_PAGE_ERR_DECODE;
    }
    qw_crypto_secure_zero(stream, sizeof(stream));

    rc = qw_ceb_decrypt_internal_pdf(
        output, layout->pdf.size,
        layout->symmetric_key.found ? bytes + layout->symmetric_key.offset : NULL,
        layout->symmetric_key.found ? layout->symmetric_key.size : 0u,
        layout->has_algorithm, layout->algorithm_id, report);
    if (rc != QW_PAGE_OK) {
        qw_crypto_secure_zero(output, layout->pdf.size);
        free(output);
        return rc;
    }
    *out_pdf = output;
    *out_size = layout->pdf.size;
    return QW_PAGE_OK;
}

static int ceb_convert_to_pdf(const void *data,
                              size_t size,
                              const QwConvertOptions *options,
                              uint8_t **out_pdf,
                              size_t *out_size,
                              QwConvertReport *report)
{
    const uint8_t *bytes = (const uint8_t *)data;
    CebLayout modern;
    CebLayout legacy;
    (void)options;
    if (!data || !out_pdf || !out_size) return QW_PAGE_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0;
    if (!ceb_probe(data, size) || size < CEB_HEADER_SIZE) {
        return QW_PAGE_ERR_FORMAT;
    }
    if (ceb_parse_modern_layout(bytes, size, &modern) &&
        ceb_layout_has_signature(bytes, size, &modern)) {
        return ceb_decrypt_to_pdf(bytes, size, &modern,
                                  out_pdf, out_size, report);
    }
    if (ceb_parse_legacy_layout(bytes, size, &legacy) &&
        ceb_layout_has_signature(bytes, size, &legacy)) {
        return ceb_decrypt_to_pdf(bytes, size, &legacy,
                                  out_pdf, out_size, report);
    }
    set_note(report,
             "CEB section layout is unsupported, incomplete, or not self-contained");
    return QW_PAGE_ERR_FORMAT;
}
#if !defined(QUIREWISE_STANDALONE_CEB)
const QwFormatDescriptor qw_format_ceb = {
    .id = QW_FORMAT_CEB,
    .name = "Founder CEB",
    .capabilities = (QW_FORMAT_CAP_PDF_DOCUMENT) | (0u),
    .probe = ceb_probe,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = ceb_convert_to_pdf
};
#endif

static QwCebStatus ceb_public_status(int status)
{
    switch (status) {
    case QW_PAGE_OK: return QW_CEB_OK;
    case QW_PAGE_ERR_INVALID_ARGUMENT: return QW_CEB_ERR_INVALID_ARGUMENT;
    case QW_PAGE_ERR_OUT_OF_MEMORY: return QW_CEB_ERR_OUT_OF_MEMORY;
    case QW_PAGE_ERR_UNSUPPORTED: return QW_CEB_ERR_UNSUPPORTED;
    case QW_PAGE_ERR_RESOURCE_LIMIT: return QW_CEB_ERR_RESOURCE_LIMIT;
    case QW_PAGE_ERR_DECODE: return QW_CEB_ERR_DECODE;
    default: return QW_CEB_ERR_FORMAT;
    }
}

QwCebStatus qw_ceb_decrypt_to_pdf(const void *data, size_t size,
                                  uint8_t **out_pdf, size_t *out_size)
{
    int status;
    if (!out_pdf || !out_size) return QW_CEB_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (size > (size_t)1024u * 1024u * 1024u)
        return QW_CEB_ERR_RESOURCE_LIMIT;
    status = ceb_convert_to_pdf(data, size, NULL, out_pdf, out_size, NULL);
    return ceb_public_status(status);
}

void qw_ceb_pdf_free(uint8_t *pdf)
{
    free(pdf);
}

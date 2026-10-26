#include "djvu_parser.h"

#include "quirewise/page_status.h"

#include <stdint.h>
#include <string.h>

static uint32_t djvu_read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint16_t djvu_read_u16_be(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static int djvu_chunk_end(const uint8_t *data,
                          size_t limit,
                          size_t offset,
                          size_t *out_end)
{
    uint32_t length;
    size_t payload_offset;

    if (!data || !out_end || offset > limit || limit - offset < 8u) {
        return 0;
    }
    length = djvu_read_u32_be(data + offset + 4u);
    payload_offset = offset + 8u;
    if ((uint64_t)length > (uint64_t)(limit - payload_offset)) {
        return 0;
    }
    *out_end = payload_offset + (size_t)length;
    return 1;
}

static int djvu_validate_form_chunks(const uint8_t *form_data,
                                    size_t form_size,
                                    const char *form_type,
                                    int require_info)
{
    size_t position = 4u;
    size_t chunk_end;
    int first = 1;

    if (!form_data || !form_type || form_size < 4u ||
        memcmp(form_data, form_type, 4u) != 0) {
        return 0;
    }
    while (position < form_size) {
        uint32_t chunk_size;
        if (!djvu_chunk_end(form_data, form_size, position, &chunk_end)) {
            return 0;
        }
        chunk_size = djvu_read_u32_be(form_data + position + 4u);
        if (first && require_info &&
            (memcmp(form_data + position, "INFO", 4u) != 0 ||
             chunk_size < 10u)) {
            return 0;
        }
        first = 0;
        if (memcmp(form_data + position, "FORM", 4u) == 0) {
            return 0;
        }
        position = chunk_end;
    }
    return !require_info || !first;
}

static int djvu_validate_page(const uint8_t *form_data, size_t form_size)
{
    return djvu_validate_form_chunks(form_data, form_size, "DJVU", 1);
}

int qw_djvu_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    if (!bytes || size < 16u || memcmp(bytes, "AT&TFORM", 8u) != 0) {
        return 0;
    }
    return memcmp(bytes + 12u, "DJVU", 4u) == 0 ||
           memcmp(bytes + 12u, "DJVM", 4u) == 0;
}

int qw_djvu_parse_page_count(const void *data,
                             size_t size,
                             size_t *out_page_count)
{
    const uint8_t *bytes = (const uint8_t *)data;
    const uint8_t *form_data;
    size_t form_size;
    size_t position;
    size_t chunk_end;
    size_t component_form_count = 0u;
    uint32_t form_length;

    if (!data || !size || !out_page_count) return QW_PAGE_ERR_INVALID_ARGUMENT;
    *out_page_count = 0u;
    if (!qw_djvu_probe(data, size)) return QW_PAGE_ERR_UNSUPPORTED;

    form_length = djvu_read_u32_be(bytes + 8u);
    if (form_length < 4u || (uint64_t)form_length != (uint64_t)(size - 12u)) {
        return QW_PAGE_ERR_FORMAT;
    }
    form_data = bytes + 12u;
    form_size = (size_t)form_length;

    if (memcmp(form_data, "DJVU", 4u) == 0) {
        if (!djvu_validate_page(form_data, form_size)) return QW_PAGE_ERR_FORMAT;
        *out_page_count = 1u;
        return QW_PAGE_OK;
    }

    if (form_size < 4u + 8u || memcmp(form_data, "DJVM", 4u) != 0) {
        return QW_PAGE_ERR_FORMAT;
    }
    position = 4u;
    if (!djvu_chunk_end(form_data, form_size, position, &chunk_end) ||
        memcmp(form_data + position, "DIRM", 4u) != 0) {
        return QW_PAGE_ERR_FORMAT;
    }
    {
        size_t directory_data = position + 8u;
        uint32_t directory_size = djvu_read_u32_be(form_data + position + 4u);
        uint8_t version_flags;
        uint16_t component_count;
        size_t offsets_size;
        int bundled;

        if (directory_size < 3u) return QW_PAGE_ERR_FORMAT;
        version_flags = form_data[directory_data];
        if ((version_flags & 0x7fu) > 1u) return QW_PAGE_ERR_UNSUPPORTED;
        bundled = (version_flags & 0x80u) != 0u;
        component_count = djvu_read_u16_be(form_data + directory_data + 1u);
        if (!component_count) return QW_PAGE_ERR_FORMAT;
        offsets_size = bundled ? (size_t)component_count * 4u : 0u;
        if (offsets_size > (size_t)directory_size - 3u) {
            return QW_PAGE_ERR_FORMAT;
        }
        if (!bundled) return QW_PAGE_ERR_UNSUPPORTED;
        position = chunk_end;

        while (position < form_size) {
            uint32_t chunk_size;
            if (!djvu_chunk_end(form_data, form_size, position, &chunk_end)) {
                return QW_PAGE_ERR_FORMAT;
            }
            chunk_size = djvu_read_u32_be(form_data + position + 4u);
            if (memcmp(form_data + position, "FORM", 4u) == 0) {
                const uint8_t *component_type;
                if (chunk_size < 4u) return QW_PAGE_ERR_FORMAT;
                component_type = form_data + position + 8u;
                ++component_form_count;
                if (memcmp(component_type, "DJVU", 4u) == 0) {
                    if (!djvu_validate_page(form_data + position + 8u,
                                            (size_t)chunk_size)) {
                        return QW_PAGE_ERR_FORMAT;
                    }
                    ++*out_page_count;
                } else if (memcmp(component_type, "DJVI", 4u) == 0 ||
                           memcmp(component_type, "THUM", 4u) == 0) {
                    const char *form_type =
                        memcmp(component_type, "DJVI", 4u) == 0
                        ? "DJVI" : "THUM";
                    if (!djvu_validate_form_chunks(
                            component_type, (size_t)chunk_size,
                            form_type, 0)) {
                        return QW_PAGE_ERR_FORMAT;
                    }
                }
            }
            position = chunk_end;
        }
        if (!*out_page_count || component_form_count != component_count ||
            *out_page_count > component_count) {
            *out_page_count = 0u;
            return QW_PAGE_ERR_FORMAT;
        }
    }
    return QW_PAGE_OK;
}

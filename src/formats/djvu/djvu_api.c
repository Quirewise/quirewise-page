#include "quirewise/djvu.h"

#include "djvu_doc.h"
#include "djvu_parser.h"
#include "format_registry.h"
#include "quirewise/page_status.h"

#include <stdlib.h>
#include <string.h>

#define QW_DJVU_MAX_INPUT ((size_t)1024u * 1024u * 1024u)
#define QW_DJVU_MAX_PAGES 10000
#define QW_DJVU_MAX_PIXELS ((size_t)100u * 1000u * 1000u)

static QwDjvuStatus djvu_map_status(int status)
{
    switch (status) {
    case QW_PAGE_OK: return QW_DJVU_OK;
    case QW_PAGE_ERR_INVALID_ARGUMENT: return QW_DJVU_ERR_INVALID_ARGUMENT;
    case QW_PAGE_ERR_UNSUPPORTED: return QW_DJVU_ERR_UNSUPPORTED;
    case QW_PAGE_ERR_RESOURCE_LIMIT: return QW_DJVU_ERR_RESOURCE_LIMIT;
    default: return QW_DJVU_ERR_FORMAT;
    }
}

uint32_t qw_djvu_abi_version(void)
{
    return QUIREWISE_DJVU_ABI_VERSION;
}

int qw_djvu_is_supported(const void *data, size_t size)
{
    return qw_djvu_probe(data, size);
}

QwDjvuStatus qw_djvu_get_page_count(const void *data, size_t size,
                                    size_t *out_page_count)
{
    size_t count = 0u;

    if (out_page_count) *out_page_count = 0u;
    if (!out_page_count) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (!data || size > QW_DJVU_MAX_INPUT) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (!qw_djvu_probe(data, size)) return QW_DJVU_ERR_UNSUPPORTED;
    {
        int rc = qw_djvu_parse_page_count(data, size, &count);
        if (rc != QW_PAGE_OK) return djvu_map_status(rc);
    }
    if (count == 0u || count > QW_DJVU_MAX_PAGES) return QW_DJVU_ERR_FORMAT;
    *out_page_count = count;
    return QW_DJVU_OK;
}

QwDjvuStatus qw_djvu_get_page_info(const void *data, size_t size,
                                   size_t page_index, QwDjvuPageInfo *out_info)
{
    QwDjvuDoc *doc = NULL;
    QwDjvuDocPageInfo info;

    if (!out_info) return QW_DJVU_ERR_INVALID_ARGUMENT;
    memset(out_info, 0, sizeof(*out_info));
    if (!data || size > QW_DJVU_MAX_INPUT) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (page_index > (size_t)QW_DJVU_MAX_PAGES) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (qw_djvu_doc_open((const uint8_t *)data, size, &doc) != 0)
        return QW_DJVU_ERR_UNSUPPORTED;
    if ((int)page_index >= qw_djvu_doc_page_count(doc)) {
        qw_djvu_doc_free(doc);
        return QW_DJVU_ERR_FORMAT;
    }
    if (qw_djvu_doc_page_info(doc, (int)page_index, &info) != 0) {
        qw_djvu_doc_free(doc);
        return QW_DJVU_ERR_FORMAT;
    }
    qw_djvu_doc_free(doc);
    out_info->width = (size_t)info.width;
    out_info->height = (size_t)info.height;
    out_info->dpi = info.dpi;
    out_info->version = info.version;
    out_info->rotation = info.rotation;
    return QW_DJVU_OK;
}

QwDjvuStatus qw_djvu_render_page(const void *data, size_t size,
                                 size_t page_index, unsigned subsample,
                                 QwDjvuImage *out_image)
{
    QwDjvuDoc *doc = NULL;
    QwDjvuDocPageInfo info;
    QwDjvuDocPageInfo check;
    uint8_t *pixels = NULL;
    size_t pixel_size = 0;
    int width = 0;
    int height = 0;
    int channels = 0;
    int rc;

    if (!out_image) return QW_DJVU_ERR_INVALID_ARGUMENT;
    memset(out_image, 0, sizeof(*out_image));
    if (!data || size > QW_DJVU_MAX_INPUT || subsample == 0u)
        return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (page_index > (size_t)QW_DJVU_MAX_PAGES) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (qw_djvu_doc_open((const uint8_t *)data, size, &doc) != 0)
        return QW_DJVU_ERR_UNSUPPORTED;
    if ((int)page_index >= qw_djvu_doc_page_count(doc)) {
        qw_djvu_doc_free(doc);
        return QW_DJVU_ERR_FORMAT;
    }
    if (qw_djvu_doc_page_info(doc, (int)page_index, &info) != 0) {
        qw_djvu_doc_free(doc);
        return QW_DJVU_ERR_FORMAT;
    }
    if ((size_t)info.width * (size_t)info.height > QW_DJVU_MAX_PIXELS) {
        qw_djvu_doc_free(doc);
        return QW_DJVU_ERR_RESOURCE_LIMIT;
    }
    rc = qw_djvu_doc_render(doc, (int)page_index, subsample, &pixels,
                            &pixel_size, &width, &height, &channels);
    if (rc == -2) {
        qw_djvu_doc_free(doc);
        return QW_DJVU_ERR_RESOURCE_LIMIT;
    }
    if (rc != 0 || !pixels || width <= 0 || height <= 0 ||
        (channels != 1 && channels != 3)) {
        free(pixels);
        qw_djvu_doc_free(doc);
        return QW_DJVU_ERR_FORMAT;
    }
    (void)check;
    out_image->width = (size_t)width;
    out_image->height = (size_t)height;
    out_image->stride = (size_t)width * (size_t)channels;
    out_image->format = channels == 3 ? QW_DJVU_PIXEL_RGB24 : QW_DJVU_PIXEL_GRAY8;
    out_image->pixels = pixels;
    out_image->size = pixel_size;
    qw_djvu_doc_free(doc);
    return QW_DJVU_OK;
}

void qw_djvu_image_free(QwDjvuImage *image)
{
    if (!image) return;
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}

QwDjvuStatus qw_djvu_extract_text(const void *data, size_t size,
                                  size_t page_index, char **out_text,
                                  size_t *out_size)
{
    QwDjvuDoc *doc = NULL;
    char *text = NULL;
    size_t text_len = 0;

    if (out_text) *out_text = NULL;
    if (out_size) *out_size = 0u;
    if (!out_text || !out_size) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (!data || size > QW_DJVU_MAX_INPUT) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (page_index > (size_t)QW_DJVU_MAX_PAGES) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (qw_djvu_doc_open((const uint8_t *)data, size, &doc) != 0)
        return QW_DJVU_ERR_UNSUPPORTED;
    if ((int)page_index >= qw_djvu_doc_page_count(doc)) {
        qw_djvu_doc_free(doc);
        return QW_DJVU_ERR_FORMAT;
    }
    if (qw_djvu_doc_text(doc, (int)page_index, &text, &text_len) != 0) {
        qw_djvu_doc_free(doc);
        text = (char *)malloc(1u);
        if (!text) return QW_DJVU_ERR_RESOURCE_LIMIT;
        text[0] = '\0';
        *out_text = text;
        *out_size = 0u;
        return QW_DJVU_OK;
    }
    qw_djvu_doc_free(doc);
    *out_text = text;
    *out_size = text_len;
    return QW_DJVU_OK;
}

void qw_djvu_text_free(char *text)
{
    free(text);
}

const char *qw_djvu_status_string(int status)
{
    switch (status) {
    case QW_DJVU_OK: return "ok";
    case QW_DJVU_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_DJVU_ERR_UNSUPPORTED: return "unsupported DjVu data";
    case QW_DJVU_ERR_FORMAT: return "invalid DjVu structure";
    case QW_DJVU_ERR_RESOURCE_LIMIT: return "DjVu resource limit exceeded";
    default: return "unknown DjVu status";
    }
}

#ifndef QUIREWISE_STANDALONE_DJVU
const QwFormatDescriptor qw_format_djvu = {
    .id = QW_FORMAT_DJVU,
    .name = "DjVu",
    .capabilities = QW_FORMAT_CAP_PAGE_IMAGES | QW_FORMAT_CAP_MULTI_PAGE,
    .probe = qw_djvu_is_supported,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL,
    .open_spreadsheet = NULL,
    .open_subtitle = NULL,
    .open_data = NULL
};
#endif

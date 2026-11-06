#include "quirewise/djvu.h"

#include "djvu_parser.h"
#include "format_registry.h"
#include "quirewise/page_status.h"
#include "vendor/djvu.h"

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

static djvu_doc *djvu_open_document(const void *data, size_t size,
                                    djvu_ctx **out_ctx)
{
    djvu_ctx *ctx;
    djvu_doc *doc;
    size_t parsed_pages = 0u;
    if (!data || size < 16u || size > QW_DJVU_MAX_INPUT) return NULL;
    if (!qw_djvu_probe(data, size)) return NULL;
    if (qw_djvu_parse_page_count(data, size, &parsed_pages) != QW_PAGE_OK)
        return NULL;
    djvu_init();
    ctx = djvu_ctx_new(NULL, NULL, NULL, NULL, NULL, NULL);
    if (!ctx) return NULL;
    doc = djvu_doc_open(ctx, (const uint8_t *)data, size);
    if (!doc) {
        djvu_ctx_free(ctx);
        return NULL;
    }
    *out_ctx = ctx;
    return doc;
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
    djvu_ctx *ctx = NULL;
    djvu_doc *doc;
    int count;
    if (out_page_count) *out_page_count = 0u;
    if (!out_page_count) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (!data || size > QW_DJVU_MAX_INPUT) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (!qw_djvu_probe(data, size)) return QW_DJVU_ERR_UNSUPPORTED;
    doc = djvu_open_document(data, size, &ctx);
    if (!doc) return QW_DJVU_ERR_FORMAT;
    count = djvu_doc_page_count(doc);
    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    if (count <= 0 || count > QW_DJVU_MAX_PAGES) return QW_DJVU_ERR_FORMAT;
    *out_page_count = (size_t)count;
    return QW_DJVU_OK;
}

QwDjvuStatus qw_djvu_get_page_info(const void *data, size_t size,
                                   size_t page_index, QwDjvuPageInfo *out_info)
{
    djvu_ctx *ctx = NULL;
    djvu_doc *doc;
    djvu_page_info info;
    if (!out_info) return QW_DJVU_ERR_INVALID_ARGUMENT;
    memset(out_info, 0, sizeof(*out_info));
    if (!data || size > QW_DJVU_MAX_INPUT) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (page_index > (size_t)QW_DJVU_MAX_PAGES) return QW_DJVU_ERR_INVALID_ARGUMENT;
    doc = djvu_open_document(data, size, &ctx);
    if (!doc) return QW_DJVU_ERR_UNSUPPORTED;
    if (djvu_doc_page_info(doc, (int)page_index, &info) != 0) {
        djvu_doc_close(doc);
        djvu_ctx_free(ctx);
        return QW_DJVU_ERR_FORMAT;
    }
    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    if (info.width <= 0 || info.height <= 0) return QW_DJVU_ERR_FORMAT;
    out_info->width = (size_t)info.width;
    out_info->height = (size_t)info.height;
    out_info->dpi = (unsigned)info.dpi;
    out_info->version = (unsigned)info.version;
    out_info->rotation = (unsigned)info.rotation;
    return QW_DJVU_OK;
}

QwDjvuStatus qw_djvu_render_page(const void *data, size_t size,
                                 size_t page_index, unsigned subsample,
                                 QwDjvuImage *out_image)
{
    djvu_ctx *ctx = NULL;
    djvu_doc *doc;
    djvu_image *image = NULL;
    QwDjvuStatus status = QW_DJVU_OK;
    if (!out_image) return QW_DJVU_ERR_INVALID_ARGUMENT;
    memset(out_image, 0, sizeof(*out_image));
    if (!data || size > QW_DJVU_MAX_INPUT || subsample == 0u)
        return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (page_index > (size_t)QW_DJVU_MAX_PAGES) return QW_DJVU_ERR_INVALID_ARGUMENT;
    doc = djvu_open_document(data, size, &ctx);
    if (!doc) return QW_DJVU_ERR_UNSUPPORTED;
    {
        djvu_page_info info;
        if (djvu_doc_page_info(doc, (int)page_index, &info) != 0 ||
            info.width <= 0 || info.height <= 0) {
            djvu_doc_close(doc);
            djvu_ctx_free(ctx);
            return QW_DJVU_ERR_FORMAT;
        }
        if ((size_t)info.width * (size_t)info.height > QW_DJVU_MAX_PIXELS) {
            djvu_doc_close(doc);
            djvu_ctx_free(ctx);
            return QW_DJVU_ERR_RESOURCE_LIMIT;
        }
    }
    image = djvu_page_render(doc, (int)page_index, (int)subsample);
    if (!image) {
        status = QW_DJVU_ERR_FORMAT;
    } else if (image->width <= 0 || image->height <= 0 ||
               (image->format != DJVU_FORMAT_GRAY8 &&
                image->format != DJVU_FORMAT_RGB24) ||
               image->stride <= 0) {
        status = QW_DJVU_ERR_FORMAT;
    } else {
        size_t row_bytes = (size_t)image->width *
                           (image->format == DJVU_FORMAT_RGB24 ? 3u : 1u);
        size_t total = row_bytes * (size_t)image->height;
        uint8_t *copy = (uint8_t *)malloc(total ? total : 1u);
        int row;
        if (!copy) {
            status = QW_DJVU_ERR_RESOURCE_LIMIT;
        } else {
            for (row = 0; row < image->height; ++row) {
                memcpy(copy + (size_t)row * row_bytes,
                       image->data + (size_t)row * (size_t)image->stride,
                       row_bytes);
            }
            out_image->width = (size_t)image->width;
            out_image->height = (size_t)image->height;
            out_image->stride = row_bytes;
            out_image->format = image->format == DJVU_FORMAT_RGB24
                                    ? QW_DJVU_PIXEL_RGB24
                                    : QW_DJVU_PIXEL_GRAY8;
            out_image->pixels = copy;
            out_image->size = total;
        }
    }
    if (image) djvu_image_destroy(ctx, image);
    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    return status;
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
    djvu_ctx *ctx = NULL;
    djvu_doc *doc;
    char *text;
    char *copy;
    if (out_text) *out_text = NULL;
    if (out_size) *out_size = 0u;
    if (!out_text || !out_size) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (!data || size > QW_DJVU_MAX_INPUT) return QW_DJVU_ERR_INVALID_ARGUMENT;
    if (page_index > (size_t)QW_DJVU_MAX_PAGES) return QW_DJVU_ERR_INVALID_ARGUMENT;
    doc = djvu_open_document(data, size, &ctx);
    if (!doc) return QW_DJVU_ERR_UNSUPPORTED;
    text = djvu_page_text(doc, (int)page_index);
    if (!text) {
        copy = (char *)malloc(1u);
        if (!copy) {
            djvu_doc_close(doc);
            djvu_ctx_free(ctx);
            return QW_DJVU_ERR_RESOURCE_LIMIT;
        }
        copy[0] = '\0';
        *out_text = copy;
        *out_size = 0u;
    } else {
        size_t length = strlen(text);
        copy = (char *)malloc(length + 1u);
        if (!copy) {
            djvu_text_destroy(ctx, text);
            djvu_doc_close(doc);
            djvu_ctx_free(ctx);
            return QW_DJVU_ERR_RESOURCE_LIMIT;
        }
        memcpy(copy, text, length + 1u);
        *out_text = copy;
        *out_size = length;
        djvu_text_destroy(ctx, text);
    }
    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
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

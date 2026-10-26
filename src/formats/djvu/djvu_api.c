#include "quirewise/djvu.h"

#include "djvu_parser.h"
#include "quirewise/page_status.h"

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
    int status;
    if (out_page_count) *out_page_count = 0u;
    if (!out_page_count) return QW_DJVU_ERR_INVALID_ARGUMENT;
    status = qw_djvu_parse_page_count(data, size, out_page_count);
    switch (status) {
    case QW_PAGE_OK: return QW_DJVU_OK;
    case QW_PAGE_ERR_INVALID_ARGUMENT: return QW_DJVU_ERR_INVALID_ARGUMENT;
    case QW_PAGE_ERR_UNSUPPORTED: return QW_DJVU_ERR_UNSUPPORTED;
    case QW_PAGE_ERR_RESOURCE_LIMIT: return QW_DJVU_ERR_RESOURCE_LIMIT;
    default: return QW_DJVU_ERR_FORMAT;
    }
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

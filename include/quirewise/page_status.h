#ifndef QUIREWISE_PAGE_STATUS_H
#define QUIREWISE_PAGE_STATUS_H

#include <stddef.h>

#if defined(_WIN32) && defined(QUIREWISE_PAGE_BUILD_SHARED)
#  if defined(QUIREWISE_PAGE_EXPORTS)
#    define QUIREWISE_PAGE_API __declspec(dllexport)
#  else
#    define QUIREWISE_PAGE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(QUIREWISE_PAGE_BUILD_SHARED)
#  define QUIREWISE_PAGE_API __attribute__((visibility("default")))
#else
#  define QUIREWISE_PAGE_API
#endif

enum {
    QW_PAGE_OK = 0,
    QW_PAGE_ERR_INVALID_ARGUMENT = -1,
    QW_PAGE_ERR_OUT_OF_MEMORY = -2,
    QW_PAGE_ERR_IO = -3,
    QW_PAGE_ERR_FORMAT = -4,
    QW_PAGE_ERR_UNSUPPORTED = -5,
    QW_PAGE_ERR_DECODE = -6,
    QW_PAGE_ERR_EMPTY = -8,
    QW_PAGE_ERR_EXTERNAL_DECODER_REQUIRED = -9,
    QW_PAGE_ERR_RESOURCE_LIMIT = -11
};

#endif

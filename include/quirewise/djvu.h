#ifndef QUIREWISE_DJVU_H
#define QUIREWISE_DJVU_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(QUIREWISE_DJVU_BUILD_SHARED)
#  if defined(QUIREWISE_DJVU_EXPORTS)
#    define QUIREWISE_DJVU_API __declspec(dllexport)
#  else
#    define QUIREWISE_DJVU_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(QUIREWISE_DJVU_BUILD_SHARED)
#  define QUIREWISE_DJVU_API __attribute__((visibility("default")))
#else
#  define QUIREWISE_DJVU_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QUIREWISE_DJVU_ABI_VERSION 1u

typedef enum QwDjvuStatus {
    QW_DJVU_OK = 0,
    QW_DJVU_ERR_INVALID_ARGUMENT = -1,
    QW_DJVU_ERR_UNSUPPORTED = -2,
    QW_DJVU_ERR_FORMAT = -3,
    QW_DJVU_ERR_RESOURCE_LIMIT = -4
} QwDjvuStatus;

QUIREWISE_DJVU_API uint32_t qw_djvu_abi_version(void);

/* Returns 1 when the input has a DjVu single-page or multipage signature. */
QUIREWISE_DJVU_API int qw_djvu_is_supported(const void *data, size_t size);

/* Validates the DjVu container structure and returns the declared page count.
 * This parses metadata only; it does not decode or render page images. */
QUIREWISE_DJVU_API QwDjvuStatus qw_djvu_get_page_count(
    const void *data, size_t size, size_t *out_page_count);
QUIREWISE_DJVU_API const char *qw_djvu_status_string(int status);

#ifdef __cplusplus
}
#endif

#endif

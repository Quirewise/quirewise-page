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

#define QUIREWISE_DJVU_ABI_VERSION 2u

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

/* Validates the DjVu container structure and returns the declared page count. */
QUIREWISE_DJVU_API QwDjvuStatus qw_djvu_get_page_count(
    const void *data, size_t size, size_t *out_page_count);

typedef struct QwDjvuPageInfo {
    size_t width;
    size_t height;
    unsigned dpi;
    unsigned version;
    unsigned rotation; /* 0, 90, 180 or 270 degrees clockwise */
} QwDjvuPageInfo;

QUIREWISE_DJVU_API QwDjvuStatus qw_djvu_get_page_info(
    const void *data, size_t size, size_t page_index, QwDjvuPageInfo *out_info);

typedef enum QwDjvuPixelFormat {
    QW_DJVU_PIXEL_GRAY8 = 1,
    QW_DJVU_PIXEL_RGB24 = 3
} QwDjvuPixelFormat;

typedef struct QwDjvuImage {
    size_t width;
    size_t height;
    size_t stride;
    QwDjvuPixelFormat format;
    uint8_t *pixels;
    size_t size;
} QwDjvuImage;

/* Renders a page to a top-down gray8 or rgb24 buffer. subsample >= 1. */
QUIREWISE_DJVU_API QwDjvuStatus qw_djvu_render_page(
    const void *data, size_t size, size_t page_index, unsigned subsample,
    QwDjvuImage *out_image);
QUIREWISE_DJVU_API void qw_djvu_image_free(QwDjvuImage *image);

/* Returns the hidden-text layer as NUL-terminated UTF-8, or "" when absent. */
QUIREWISE_DJVU_API QwDjvuStatus qw_djvu_extract_text(
    const void *data, size_t size, size_t page_index, char **out_text,
    size_t *out_size);
QUIREWISE_DJVU_API void qw_djvu_text_free(char *text);

QUIREWISE_DJVU_API const char *qw_djvu_status_string(int status);

#ifdef __cplusplus
}
#endif

#endif

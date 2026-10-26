#ifndef QUIREWISE_CEB_H
#define QUIREWISE_CEB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwCebStatus {
    QW_CEB_OK = 0,
    QW_CEB_ERR_INVALID_ARGUMENT = -1,
    QW_CEB_ERR_OUT_OF_MEMORY = -2,
    QW_CEB_ERR_FORMAT = -3,
    QW_CEB_ERR_UNSUPPORTED = -4,
    QW_CEB_ERR_RESOURCE_LIMIT = -5,
    QW_CEB_ERR_DECODE = -6
} QwCebStatus;

/* The result is a complete PDF buffer owned by the caller. */
QwCebStatus qw_ceb_decrypt_to_pdf(const void *data, size_t size,
                                  uint8_t **out_pdf, size_t *out_size);
void qw_ceb_pdf_free(uint8_t *pdf);

#ifdef __cplusplus
}
#endif

#endif

#ifndef QUIREWISE_UVZ_PARSER_H
#define QUIREWISE_UVZ_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwUvzStatus {
    QW_UVZ_OK = 0,
    QW_UVZ_ERR_INVALID_ARGUMENT = -1,
    QW_UVZ_ERR_OUT_OF_MEMORY = -2,
    QW_UVZ_ERR_FORMAT = -3,
    QW_UVZ_ERR_UNSUPPORTED = -4,
    QW_UVZ_ERR_RESOURCE_LIMIT = -5,
    QW_UVZ_ERR_DECODE = -6
} QwUvzStatus;

typedef struct QwUvzDocument QwUvzDocument;

typedef struct QwUvzPageInfo {
    const char *name;
    uint64_t uncompressed_size;
    int is_blank;
} QwUvzPageInfo;

/* Input ZIP bytes are borrowed and must remain valid until document close. */
QwUvzStatus qw_uvz_document_open(const void *data, size_t size,
                                 QwUvzDocument **out_document);
size_t qw_uvz_document_page_count(const QwUvzDocument *document);
QwUvzStatus qw_uvz_document_page_info(const QwUvzDocument *document,
                                      size_t page_index,
                                      QwUvzPageInfo *out_info);
/* Extracted page bytes are allocated by the library and freed with
 * qw_uvz_page_data_free. */
QwUvzStatus qw_uvz_document_read_page(QwUvzDocument *document,
                                      size_t page_index,
                                      uint8_t **out_data,
                                      size_t *out_size);
void qw_uvz_page_data_free(uint8_t *data);
void qw_uvz_document_close(QwUvzDocument *document);

#ifdef __cplusplus
}
#endif

#endif

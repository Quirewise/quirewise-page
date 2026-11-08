#ifndef QUIREWISE_CEB_INTERNAL_H
#define QUIREWISE_CEB_INTERNAL_H

#include "quirewise/format.h"
#include "quirewise/page_status.h"
#include "../../core/byte_utils.h"

int qw_ceb_decrypt_internal_pdf(uint8_t *pdf,
                                size_t pdf_size,
                                const uint8_t *key_blob,
                                size_t key_blob_size,
                                int has_algorithm,
                                uint32_t algorithm,
                                QwConvertReport *report);

#endif

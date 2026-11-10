/* First-party DjVu IFF container reader. */

#include "djvu_iff.h"

#include <string.h>

static uint32_t iff_read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

int qw_djvu_iff_form(const uint8_t *data, size_t size, QwDjvuIffForm *out)
{
    uint32_t length;

    if (!data || !out || size < 12u) return -1;
    if (memcmp(data, "AT&TFORM", 8u) != 0) return -1;
    length = iff_read_u32(data + 8);
    if ((size_t)length > size - 12u || length < 4u) return -1;
    out->data = data + 12 + 4;
    out->size = (size_t)length - 4u;
    memcpy(out->type, data + 12, 4u);
    out->type[4] = '\0';
    return 0;
}

int qw_djvu_iff_nested(const QwDjvuIffChunk *chunk, QwDjvuIffForm *out)
{
    if (!chunk || !out || chunk->size < 4u) return -1;
    out->data = chunk->data + 4;
    out->size = chunk->size - 4u;
    memcpy(out->type, chunk->data, 4u);
    out->type[4] = '\0';
    return 0;
}

int qw_djvu_iff_next(const QwDjvuIffForm *form, size_t *cursor,
                     QwDjvuIffChunk *out)
{
    size_t position;
    uint32_t length;

    if (!form || !cursor || !out) return -1;
    position = *cursor;
    if (position >= form->size) return 0;
    if (form->size - position < 8u) return -1;
    length = iff_read_u32(form->data + position + 4);
    if ((size_t)length > form->size - position - 8u) return -1;

    memcpy(out->id, form->data + position, 4u);
    out->id[4] = '\0';
    out->data = form->data + position + 8u;
    out->size = (size_t)length;

    position += 8u + (size_t)length;
    if ((length & 1u) != 0u && position < form->size) ++position;
    *cursor = position;
    return 1;
}

int qw_djvu_iff_find(const QwDjvuIffForm *form, const char *id,
                     QwDjvuIffChunk *out)
{
    size_t cursor = 0;
    QwDjvuIffChunk chunk;
    int rc;

    if (!form || !id || strlen(id) != 4u) return -1;
    while ((rc = qw_djvu_iff_next(form, &cursor, &chunk)) == 1) {
        if (memcmp(chunk.id, id, 4u) == 0) {
            if (out) *out = chunk;
            return 1;
        }
    }
    return rc < 0 ? -1 : 0;
}

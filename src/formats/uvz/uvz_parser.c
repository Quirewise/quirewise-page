#include "quirewise/uvz_parser.h"

#include "../archive/zip_native.h"
#include "../../core/natural_sort.h"

#include <stdlib.h>
#include <string.h>

typedef struct QwUvzPageEntry {
    size_t archive_index;
    char *name;
    uint64_t uncompressed_size;
} QwUvzPageEntry;

struct QwUvzDocument {
    QwZipArchive archive;
    QwUvzPageEntry *pages;
    size_t page_count;
    int archive_open;
};

static unsigned char uvz_ascii_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
        ? (unsigned char)(value - 'A' + 'a') : value;
}

static int uvz_equal_prefix(const char *text, const char *prefix)
{
    while (*prefix) {
        if (!*text || uvz_ascii_lower((unsigned char)*text) !=
                      uvz_ascii_lower((unsigned char)*prefix)) return 0;
        ++text;
        ++prefix;
    }
    return 1;
}

static const char *uvz_basename(const char *name)
{
    const char *base = name;
    const char *cursor;
    for (cursor = name; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1;
    }
    return base;
}

/* Length of a basename without its final extension. */
static size_t uvz_stem_length(const char *base)
{
    const char *dot = strrchr(base, '.');
    return (dot && dot != base) ? (size_t)(dot - base) : strlen(base);
}

static unsigned uvz_entry_rank(const char *name)
{
    const char *stem = uvz_basename(name);
    size_t length = uvz_stem_length(stem);
    int first;
    if (length == 6u && uvz_equal_prefix(stem, "cov001")) return 0u;
    if (uvz_equal_prefix(stem, "bok")) return 1u;
    if (uvz_equal_prefix(stem, "leg")) return 2u;
    if (stem[0] == '!') return 3u;
    if (uvz_equal_prefix(stem, "fow") || uvz_equal_prefix(stem, "pre"))
        return 4u;
    first = (unsigned char)stem[0];
    if (first >= '0' && first <= '9') return 10u;
    if (uvz_equal_prefix(stem, "att") || uvz_equal_prefix(stem, "app"))
        return 80u;
    if (uvz_equal_prefix(stem, "bac") ||
        (length == 6u && uvz_equal_prefix(stem, "cov002"))) return 90u;
    return 50u;
}

/* SSReader containers store one page per member named NNNNNN.pdg; older rips
 * used the same layout with the ".page" alias. Everything else in the archive
 * (Bookinfo.dat, BookContents.dat, ssct.data) is metadata, not a page. */
static int uvz_has_page_suffix(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) {
        return 0;
    }
    ++dot;
    if (uvz_ascii_lower((unsigned char)dot[0]) == 'p' &&
        uvz_ascii_lower((unsigned char)dot[1]) == 'd' &&
        uvz_ascii_lower((unsigned char)dot[2]) == 'g' && dot[3] == '\0') {
        return 1;
    }
    if (uvz_ascii_lower((unsigned char)dot[0]) == 'p' &&
        uvz_ascii_lower((unsigned char)dot[1]) == 'a' &&
        uvz_ascii_lower((unsigned char)dot[2]) == 'g' &&
        uvz_ascii_lower((unsigned char)dot[3]) == 'e' && dot[4] == '\0') {
        return 1;
    }
    return 0;
}

static int uvz_compare_pages(const void *left, const void *right)
{
    const QwUvzPageEntry *a = (const QwUvzPageEntry *)left;
    const QwUvzPageEntry *b = (const QwUvzPageEntry *)right;
    unsigned ar = uvz_entry_rank(a->name);
    unsigned br = uvz_entry_rank(b->name);
    int order = (ar > br) - (ar < br);
    if (!order) order = qw_page_natural_name_compare(a->name, b->name);
    if (!order) order = (a->archive_index > b->archive_index) -
                        (a->archive_index < b->archive_index);
    return order;
}

static char *uvz_get_entry_name(QwZipArchive *archive, size_t index)
{
    size_t required = qw_zip_reader_get_filename(archive, index, NULL, 0u);
    char *name;
    if (!required || required == (size_t)-1 || required == SIZE_MAX) return NULL;
    name = (char *)malloc(required + 1u);
    if (!name) return NULL;
    if (!qw_zip_reader_get_filename(archive, index, name, required + 1u)) {
        free(name);
        return NULL;
    }
    name[required] = '\0';
    return name;
}

static void uvz_document_release_contents(QwUvzDocument *document)
{
    if (!document) return;
    for (size_t i = 0u; i < document->page_count; ++i)
        free(document->pages[i].name);
    free(document->pages);
    document->pages = NULL;
    document->page_count = 0u;
    if (document->archive_open) {
        qw_zip_reader_end(&document->archive);
        document->archive_open = 0;
    }
}

QwUvzStatus qw_uvz_document_open(const void *data, size_t size,
                                 QwUvzDocument **out_document)
{
    QwUvzDocument *document;
    size_t archive_count;
    size_t capacity = 0u;
    if (!out_document) return QW_UVZ_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (!data || !size) return QW_UVZ_ERR_INVALID_ARGUMENT;
    document = (QwUvzDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_UVZ_ERR_OUT_OF_MEMORY;
    if (!qw_zip_reader_init_mem_with_profile(
            &document->archive, data, size, 0u,
            QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE)) {
        free(document);
        return QW_UVZ_ERR_FORMAT;
    }
    document->archive_open = 1;
    archive_count = qw_zip_reader_get_num_files(&document->archive);
    if (archive_count > SIZE_MAX / sizeof(*document->pages)) {
        qw_uvz_document_close(document);
        return QW_UVZ_ERR_RESOURCE_LIMIT;
    }
    if (archive_count) {
        document->pages = (QwUvzPageEntry *)calloc(
            archive_count, sizeof(*document->pages));
        if (!document->pages) {
            qw_uvz_document_close(document);
            return QW_UVZ_ERR_OUT_OF_MEMORY;
        }
        capacity = archive_count;
    }
    for (size_t i = 0u; i < archive_count; ++i) {
        char *name;
        QwZipEntryInfo info;
        if (qw_zip_reader_is_file_a_directory(&document->archive, i)) continue;
        name = uvz_get_entry_name(&document->archive, i);
        if (!name) {
            qw_uvz_document_close(document);
            return QW_UVZ_ERR_OUT_OF_MEMORY;
        }
        if (!uvz_has_page_suffix(name)) {
            free(name);
            continue;
        }
        if (!qw_zip_reader_file_stat(&document->archive, i, &info)) {
            free(name);
            qw_uvz_document_close(document);
            return QW_UVZ_ERR_FORMAT;
        }
        if (document->page_count >= capacity) {
            free(name);
            qw_uvz_document_close(document);
            return QW_UVZ_ERR_RESOURCE_LIMIT;
        }
        document->pages[document->page_count].archive_index = i;
        document->pages[document->page_count].name = name;
        document->pages[document->page_count].uncompressed_size =
            info.uncompressed_size;
        ++document->page_count;
    }
    if (!document->page_count) {
        qw_uvz_document_close(document);
        return QW_UVZ_ERR_FORMAT;
    }
    qsort(document->pages, document->page_count, sizeof(*document->pages),
          uvz_compare_pages);
    *out_document = document;
    return QW_UVZ_OK;
}

size_t qw_uvz_document_page_count(const QwUvzDocument *document)
{
    return document ? document->page_count : 0u;
}

QwUvzStatus qw_uvz_document_page_info(const QwUvzDocument *document,
                                      size_t page_index,
                                      QwUvzPageInfo *out_info)
{
    const QwUvzPageEntry *page;
    if (!document || !out_info || page_index >= document->page_count)
        return QW_UVZ_ERR_INVALID_ARGUMENT;
    page = &document->pages[page_index];
    out_info->name = page->name;
    out_info->uncompressed_size = page->uncompressed_size;
    out_info->is_blank = page->uncompressed_size == 0u;
    return QW_UVZ_OK;
}

QwUvzStatus qw_uvz_document_read_page(QwUvzDocument *document,
                                      size_t page_index,
                                      uint8_t **out_data,
                                      size_t *out_size)
{
    const QwUvzPageEntry *page;
    size_t extracted_size = 0u;
    uint8_t *data;
    if (!document || !out_data || !out_size ||
        page_index >= document->page_count)
        return QW_UVZ_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    page = &document->pages[page_index];
    if (page->uncompressed_size > SIZE_MAX) return QW_UVZ_ERR_RESOURCE_LIMIT;
    if (!page->uncompressed_size) return QW_UVZ_OK;
    if (!qw_zip_reader_is_file_supported(&document->archive,
                                         page->archive_index))
        return QW_UVZ_ERR_UNSUPPORTED;
    data = (uint8_t *)qw_zip_reader_extract_to_heap(
        &document->archive, page->archive_index, &extracted_size, 0u);
    if (!data) return QW_UVZ_ERR_DECODE;
    if (extracted_size != page->uncompressed_size) {
        free(data);
        return QW_UVZ_ERR_FORMAT;
    }
    *out_data = data;
    *out_size = extracted_size;
    return QW_UVZ_OK;
}

void qw_uvz_page_data_free(uint8_t *data)
{
    free(data);
}

void qw_uvz_document_close(QwUvzDocument *document)
{
    if (!document) return;
    uvz_document_release_contents(document);
    free(document);
}

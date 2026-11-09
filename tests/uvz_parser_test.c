/* Coverage for the UVZ container parser: a real SSReader container stores one
 * .pdg per page and keeps Bookinfo.dat / BookContents.dat next to them, so the
 * page filter must accept .pdg (and the legacy .page alias) and ignore the
 * metadata members. The fixture is a synthetic ZIP generated for this test. */

#include <quirewise/uvz_parser.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/core/format_registry.h"

#include "uvz_fixture.h"

static int g_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

/* Expected page order: cover, title page, table of contents, foreword, body,
 * attachment, back cover - the SSReader member naming convention. */
static const char *const kExpectedOrder[] = {
    "cov001", "bok001", "fow001", "!00001", "!00002",
    "000001", "000002", "000003", "000004", "000005",
    "000006", "000007", "000008", "000009", "000010", "000011",
    "att001", "cov002",
};

static const char *const kExpectedBytes[] = {
    "COVER", "TITLE", "FORE00001", "TOC00001", "TOC00002",
    "PAGE000001", "PAGE000002", "PAGE000003", "PAGE000004", "PAGE000005",
    "PAGE000006", "PAGE000007", "PAGE000008", "PAGE000009", "PAGE000010",
    "PAGE000011",
    "ATT00001", "BACK",
};

static void test_page_order_and_bytes(void)
{
    QwUvzDocument *document = NULL;
    size_t count;
    size_t index;
    size_t expected = sizeof(kExpectedOrder) / sizeof(kExpectedOrder[0]);

    CHECK(qw_uvz_document_open(kUvzFixture, sizeof(kUvzFixture), &document) == QW_UVZ_OK);
    CHECK(document != NULL);
    if (!document) {
        return;
    }
    count = qw_uvz_document_page_count(document);
    CHECK(count == expected);
    for (index = 0u; index < count && index < expected; ++index) {
        QwUvzPageInfo info;
        uint8_t *data = NULL;
        size_t size = 0u;
        const char *name;
        memset(&info, 0, sizeof(info));
        CHECK(qw_uvz_document_page_info(document, index, &info) == QW_UVZ_OK);
        name = info.name ? strrchr(info.name, '/') : NULL;
        name = name ? name + 1 : info.name;
        if (!name) {
            fprintf(stderr, "page %zu has no name\n", index);
            ++g_failures;
            continue;
        }
        if (strncmp(name, kExpectedOrder[index], strlen(kExpectedOrder[index])) != 0) {
            fprintf(stderr, "page %zu: expected %s*, got %s\n",
                    index, kExpectedOrder[index], name);
            ++g_failures;
        }
        CHECK(qw_uvz_document_read_page(document, index, &data, &size) == QW_UVZ_OK);
        {
            size_t want = strlen(kExpectedBytes[index]);
            if (!data || size != want ||
                memcmp(data, kExpectedBytes[index], want) != 0) {
                fprintf(stderr, "page %zu: payload mismatch (got %zu bytes)\n",
                        index, size);
                ++g_failures;
            }
        }
        CHECK(size == info.uncompressed_size);
        qw_uvz_page_data_free(data);
    }
    qw_uvz_document_close(document);
}

static void test_invalid_input(void)
{
    static const unsigned char junk[] = {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u,
                                         0x06u, 0x07u, 0x08u, 0x09u};
    QwUvzDocument *document = NULL;
    CHECK(qw_uvz_document_open(NULL, 0u, &document) == QW_UVZ_ERR_INVALID_ARGUMENT);
    CHECK(document == NULL);
    CHECK(qw_uvz_document_open(kUvzFixture, 0u, &document) == QW_UVZ_ERR_INVALID_ARGUMENT);
    CHECK(qw_uvz_document_open(junk, sizeof(junk), &document) == QW_UVZ_ERR_FORMAT);
    CHECK(qw_uvz_document_page_count(NULL) == 0u);
    qw_uvz_document_close(NULL);
    qw_uvz_page_data_free(NULL);
}

static void test_format_descriptor(void)
{
    CHECK(qw_format_uvz.id == QW_FORMAT_UVZ);
    CHECK(qw_format_uvz.name != NULL);
    CHECK(qw_format_uvz.probe != NULL);
    CHECK(qw_format_uvz.probe(kUvzFixture, sizeof(kUvzFixture)) == 1);
    CHECK(qw_format_uvz.probe("not a container", 15u) == 0);
    CHECK(qw_format_uvz.probe(kUvzFixture, 0u) == 0);
    CHECK(qw_format_uvz.probe(NULL, 0u) == 0);
}

int main(void)
{
    test_page_order_and_bytes();
    test_invalid_input();
    test_format_descriptor();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("PASS: UVZ container pages");
    return 0;
}

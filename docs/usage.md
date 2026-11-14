# quirewise-page usage guide

This guide covers the task-level workflows of `Quirewise::Page`. There is no
single common page interface: each format has its own handle, status enum and
release function under `quirewise/`.

## 1. Build and integrate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Consumer CMake:

```cmake
find_package(QuirewisePage CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Page)
```

All entry points take a memory buffer and its size. The library never performs
file I/O, so load the container first and keep the bytes alive as noted below.

## 2. Core model and ownership

| Format | Handle | Input lifetime | Output ownership |
|---|---|---|---|
| DjVu | none (stateless calls) | call duration | `qw_djvu_image_free`, `qw_djvu_text_free` |
| UVZ | `QwUvzDocument` | until `qw_uvz_document_close` | `qw_uvz_page_data_free` for page bytes |
| CAJ | `QwCajDocument` | until `qw_caj_document_close` | library handle; figures/text borrowed |
| CEB | none (stateless call) | call duration | `qw_ceb_pdf_free` |

- Input bytes are borrowed. No output points into caller memory except CAJ
  figure and text pointers, which point into the CAJ input.
- Every allocation has a documented release function, and output structs should
  start zero-initialized; release functions accept NULL.
- Status codes are negative on failure; `*_OK` is `0`. Only DjVu offers a
  `qw_djvu_status_string` helper, so switch on the enum for the other formats.

## 3. DjVu: page count, info, render and text

DjVu calls are stateless; the same buffer is parsed once per call.

```c
#include <quirewise/djvu.h>

QwDjvuPageInfo info;
QwDjvuImage image = {0};
char *text = NULL;
size_t text_size = 0, pages = 0;
QwDjvuStatus st;

st = qw_djvu_get_page_count(data, size, &pages);
if (st == QW_DJVU_OK) st = qw_djvu_get_page_info(data, size, 0, &info);
if (st == QW_DJVU_OK) st = qw_djvu_render_page(data, size, 0, 1, &image);
if (st == QW_DJVU_OK)
    st = qw_djvu_extract_text(data, size, 0, &text, &text_size);
if (st != QW_DJVU_OK) {
    fprintf(stderr, "%s\n", qw_djvu_status_string(st));
    return 1;
}
printf("page 0: %zux%zu dpi=%u rotation=%u; rendered %zux%zu %s\n",
       info.width, info.height, info.dpi, info.rotation,
       image.width, image.height,
       image.format == QW_DJVU_PIXEL_RGB24 ? "rgb24" : "gray8");
printf("text: %zu bytes\n", text_size);
qw_djvu_text_free(text);
qw_djvu_image_free(&image);
```

- `subsample` is an integer ≥ 1. `2` renders at roughly half resolution; the
  result is `ceil(width / subsample)` by `ceil(height / subsample)`.
- `info.rotation` is metadata (0/90/180/270), not applied to the rendered
  buffer. Rotate the pixels yourself if the page needs it.
- `qw_djvu_extract_text` returns an allocated NUL-terminated UTF-8 string. A
  page without a text layer yields `""` and `out_size == 0`, still `QW_DJVU_OK`.

## 4. UVZ: list pages and extract PDG bytes

UVZ is a ZIP whose page members are named `NNNNNN.pdg` (or the legacy `.page`).
The parser orders them by SSReader naming: cover, title, front matter,
table of contents, body pages, attachments, back cover.

```c
#include <quirewise/uvz_parser.h>
#include <pdg.h>

QwUvzDocument *document = NULL;
size_t page_count, index;

if (qw_uvz_document_open(data, size, &document) != QW_UVZ_OK) return 1;
page_count = qw_uvz_document_page_count(document);
for (index = 0; index < page_count; ++index) {
    QwUvzPageInfo info;
    uint8_t *page = NULL;
    size_t page_size = 0;
    PdgBuffer decoded = {0};

    if (qw_uvz_document_page_info(document, index, &info) != QW_UVZ_OK) break;
    printf("%zu: %s (%llu bytes)%s\n", index, info.name,
           (unsigned long long)info.uncompressed_size,
           info.is_blank ? " [blank]" : "");
    if (qw_uvz_document_read_page(document, index, &page, &page_size) !=
        QW_UVZ_OK)
        break;
    if (page && pdg_decode_memory(page, page_size, &decoded) == PDG_OK)
        pdg_buffer_free(&decoded);
    qw_uvz_page_data_free(page);
}
qw_uvz_document_close(document);
```

The bytes are PDG page payloads; this package does not decode them. Link
`Quirewise::PDG` for `pdg_decode_memory` or use another decoder. `Bookinfo.dat`,
`BookContents.dat` and `ssct.data` are metadata, excluded from the page list.

## 5. CAJ: pages, figures, encoded text and outlines

`qw_caj_document_open` borrows the input buffer. Page tables, figure pointers and
text pointers stay valid until `qw_caj_document_close`.

```c
#include <quirewise/caj.h>
QwCajDocument *document = NULL;
QwCajPageInfo page_info;
QwCajFigureInfo figure;
const uint8_t *text = NULL;
size_t text_size = 0, page_count, page;

if (qw_caj_document_open(data, size, &document) != QW_CAJ_OK) return 1;
page_count = qw_caj_document_page_count(document);
for (page = 0; page < page_count; ++page) {
    size_t f;
    if (qw_caj_document_page_info(document, page, &page_info) != QW_CAJ_OK)
        break;
    for (f = 0; f < page_info.figure_count; ++f) {
        if (qw_caj_document_figure_info(document, page, f, &figure) !=
            QW_CAJ_OK)
            continue;
        /* figure.kind, figure.data, figure.size, figure.x, figure.y */
    }
    if (qw_caj_document_page_text(document, page, &text, &text_size) ==
        QW_CAJ_OK) {
        /* encoded record; may contain a COMPRESSTEXT block */
    }
}
qw_caj_document_close(document);
```

`QwCajFigureKind` names the payload that `figure.data` points at: private JBIG,
raster, bottom-up raster, JBIG2 or JPEG 2000. None of them is decoded here.

Classic CAJ files have a separate outline and PDF path:

```c
QwCajClassicOutlineList *outlines = NULL;
QwCajClassicOutlineInfo outline;

if (qw_caj_classic_outlines_parse(data, size, &outlines) == QW_CAJ_OK) {
    size_t count = qw_caj_classic_outline_count(outlines);
    for (size_t i = 0; i < count; ++i) {
        if (qw_caj_classic_outline_get(outlines, i, &outline) != QW_CAJ_OK)
            continue;
        /* title_bytes is GBK, borrowed until the list is freed */
        printf("page %zu depth %zu\n", outline.page_index, outline.depth);
    }
    qw_caj_classic_outlines_free(outlines);
}
```

## 6. Recover an embedded PDF (CEB and CAJ)

CEB decryption returns a complete PDF; the bytes are owned by the caller.

```c
#include <quirewise/ceb.h>
uint8_t *pdf = NULL;
size_t pdf_size = 0;
if (qw_ceb_decrypt_to_pdf(data, size, &pdf, &pdf_size) == QW_CEB_OK) {
    fwrite(pdf, 1, pdf_size, stdout);   /* or save to disk */
    qw_ceb_pdf_free(pdf);
}
```

CAJ has two PDF routes. Classic CAJ carries a PDF body inside the container;
KDH files carry an XOR-obfuscated PDF after a 254-byte header.

```c
uint8_t *pdf = NULL;
size_t pdf_size = 0;
QwCajStatus st = qw_caj_extract_classic_pdf(data, size, &pdf, &pdf_size);
if (st == QW_CAJ_ERR_UNSUPPORTED)
    st = qw_caj_decrypt_kdh_pdf(data, size, &pdf, &pdf_size);
if (st == QW_CAJ_OK) {
    /* consume pdf[0..pdf_size) */
    qw_caj_pdf_data_free(pdf);
}
```

Both functions validate the `%PDF-` signature before returning. Rendering,
merging or rewriting these PDFs belongs to `Quirewise::Document`.

## 7. Error handling

| Meaning | DjVu | UVZ | CAJ | CEB |
|---|---|---|---|---|
| Bad argument or index | `QW_DJVU_ERR_INVALID_ARGUMENT` | `QW_UVZ_ERR_INVALID_ARGUMENT` | `QW_CAJ_ERR_INVALID_ARGUMENT` | `QW_CEB_ERR_INVALID_ARGUMENT` |
| Not this container | `QW_DJVU_ERR_UNSUPPORTED` | `QW_UVZ_ERR_FORMAT` | `QW_CAJ_ERR_UNSUPPORTED` | `QW_CEB_ERR_FORMAT` |
| Structural damage | `QW_DJVU_ERR_FORMAT` | `QW_UVZ_ERR_FORMAT` | `QW_CAJ_ERR_FORMAT` | `QW_CEB_ERR_FORMAT` |
| Limit exceeded | `QW_DJVU_ERR_RESOURCE_LIMIT` | `QW_UVZ_ERR_RESOURCE_LIMIT` | — | `QW_CEB_ERR_RESOURCE_LIMIT` |
| Decode failure | — | `QW_UVZ_ERR_DECODE` | `QW_CAJ_ERR_DECODE` | `QW_CEB_ERR_DECODE` |

An out-of-range `page_index` on DjVu maps to `QW_DJVU_ERR_FORMAT` (the container
is parsed first), while UVZ and CAJ report `*_ERR_INVALID_ARGUMENT`, so check
the page count before indexing.

## 8. Memory, lifetime and threading

- DjVu keeps the buffer only for the call; UVZ and CAJ retain it until close, so
  reallocating or modifying it first invalidates page, figure and text pointers.
- `qw_uvz_document_read_page` allocates page bytes; free them with
  `qw_uvz_page_data_free`, including blank pages (NULL, size 0).
- CAJ figures and text are borrowed from the input; never free them separately.
- There is no global mutable state, but a single document is not thread-safe.
  Use one document per thread; stateless DjVu/CEB calls may run concurrently.

## 9. Limits and format notes

- DjVu: 1 GiB input, 10,000 pages, 100,000,000 rendered pixels. Only bundled
  `DJVM` multipage files are supported. A page form needs an Iw44 `BG44` or a
  JB2 `Sjbz` chunk to render; other forms report `QW_DJVU_ERR_FORMAT`.
- DjVu `INFO` rotation is reported but not applied, and DPI is metadata only.
- UVZ page order is convention-based, not oracle-verified. Blank members are
  reported through `is_blank`; a zero-byte page reads back as NULL/0.
- CAJ page-list limits: 100,000 pages and 4,096 figures per page; a
  `COMPRESSTEXT` section expanding past 256 MiB is rejected. The classic `CAJ`
  header is not supported by the page-list API, only by the classic PDF and
  outline helpers.
- CEB input is capped at 1 GiB and must contain a self-contained section layout;
  otherwise the call reports `QW_CEB_ERR_FORMAT`.
- Nothing in this package parses, renders, merges or writes PDF.

## 10. Related packages

- `Quirewise::PDG` decodes UVZ and CAJ page payloads and PDZ containers.
- `Quirewise::Document` reads, renders and merges the PDFs recovered here.
- `Quirewise::Image` decodes figures once they are ordinary JPEG/PNG bytes.
- `Quirewise::Archive` provides the ZIP reader used by the UVZ parser.

# quirewise-page

Page-level access to DjVu, SSReader UVZ, CAJ and Founder CEB: enumerate pages,
read page metadata and pixels, extract text and outlines, recover PDFs.

`Quirewise::Page` is an independent, MIT-licensed C11 library, installed as the
CMake package `QuirewisePage`; it does not require the Quirewise application.

## What it does

- **DjVu**: recognises single-page `DJVU` and bundled multipage `DJVM` files,
  reports page count and geometry/DPI/rotation, renders a page to Gray8 or
  RGB24 at an integer subsample, and extracts the hidden-text layer.
- **UVZ**: opens an SSReader ZIP container in memory, builds the page list in
  SSReader order (cover, title, front matter, table of contents, body pages,
  attachments, back cover), and extracts one page's bytes.
- **CAJ**: lists pages and figures, exposes the encoded text record, parses
  classic-CAJ outlines, extracts an embedded classic PDF, and decrypts KDH PDF
  payloads.
- **CEB**: decrypts a Founder CEB container to a complete PDF buffer.

The package is a page/container layer, not a PDF or PDG codec. It does not
parse, render or merge PDF; `Quirewise::Document` owns that. UVZ and CAJ return
page bytes, not decoded images: use `Quirewise::PDG` for PDG pages.

## Requirements

- C11 compiler and CMake 3.21 or newer; installed `QuirewiseRuntime`,
  `QuirewiseArchive`, `QuirewiseCrypto` and `QuirewiseCompression` packages.
- No third-party libraries; CTest drives the tests.

## Build, test and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Shared builds are available with `-DQUIREWISE_BUILD_SHARED=ON`.

## Use it from CMake

```cmake
find_package(QuirewisePage CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Page)
```

Add `-DCMAKE_PREFIX_PATH=/path/to/prefix` for a non-default install prefix.

## Quick start

Render page 0 of a DjVu file and report its size:

```c
#include <quirewise/djvu.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data;
    long length;
    if (!file || fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file) fclose(file);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    QwDjvuImage image = {0};
    QwDjvuPageInfo info = {0};
    size_t pages = 0;
    size_t size = 0;
    unsigned char *data;
    QwDjvuStatus status;

    if (argc != 2) return 1;
    data = read_file(argv[1], &size);
    if (!data || !qw_djvu_is_supported(data, size)) {
        free(data);
        return 1;
    }
    status = qw_djvu_get_page_count(data, size, &pages);
    if (status == QW_DJVU_OK) status = qw_djvu_get_page_info(data, size, 0, &info);
    if (status == QW_DJVU_OK)
        status = qw_djvu_render_page(data, size, 0, 1, &image);
    free(data);
    if (status != QW_DJVU_OK) {
        fprintf(stderr, "%s\n", qw_djvu_status_string(status));
        return 1;
    }
    printf("%zu page(s); page 0 %zux%zu dpi=%u rotation=%u %s\n",
           pages, image.width, image.height, info.dpi, info.rotation,
           image.format == QW_DJVU_PIXEL_RGB24 ? "rgb24" : "gray8");
    qw_djvu_image_free(&image);
    return 0;
}
```

## API overview

| Header | Purpose |
|---|---|
| `quirewise/djvu.h` | DjVu probe, page count/info, render, hidden text |
| `quirewise/uvz_parser.h` | SSReader UVZ page list and page extraction |
| `quirewise/caj.h` | CAJ pages, figures, text, outlines, PDF recovery |
| `quirewise/ceb.h` | Founder CEB to PDF decryption |
| `quirewise/page_status.h` | shared `QW_PAGE_*` status codes for the document layer |

| Task | Functions |
|---|---|
| DjVu | `qw_djvu_is_supported`, `qw_djvu_get_page_count`, `qw_djvu_get_page_info`, `qw_djvu_render_page`, `qw_djvu_extract_text` |
| UVZ | `qw_uvz_document_open`, `qw_uvz_document_page_count`, `qw_uvz_document_page_info`, `qw_uvz_document_read_page` |
| CAJ | `qw_caj_document_open`, `qw_caj_document_page_count`, `qw_caj_document_page_info`, `qw_caj_document_figure_info`, `qw_caj_document_page_text` |
| CAJ outlines / PDF | `qw_caj_classic_outlines_parse`, `qw_caj_classic_outline_get`, `qw_caj_extract_classic_pdf`, `qw_caj_decrypt_kdh_pdf` |
| Release | `qw_djvu_image_free`, `qw_djvu_text_free`, `qw_uvz_document_close`, `qw_uvz_page_data_free`, `qw_caj_document_close`, `qw_caj_classic_outlines_free`, `qw_caj_pdf_data_free`, `qw_ceb_pdf_free` |
| CEB | `qw_ceb_decrypt_to_pdf` |

## Behavior and limits

- Each format has its own negative status enum; `QW_DJVU_OK`, `QW_UVZ_OK`,
  `QW_CAJ_OK` and `QW_CEB_OK` are `0`. Only DjVu has a status-string helper.
- DjVu limits: 1 GiB input, 10,000 pages, 100,000,000 rendered pixels. Only
  bundled `DJVM` directories work; indirect multipage files report
  `QW_DJVU_ERR_UNSUPPORTED`. Rendering handles `BG44` (Iw44) and `Sjbz` (JB2).
- DjVu `subsample` (>= 1) is nearest-neighbour; output is `ceil(width / subsample)`
  pixels and `rotation` is reported, not applied.
- `qw_djvu_extract_text` succeeds for a valid page; no text layer yields `""`.
- CEB output is a complete PDF owned by the caller and released with
  `qw_ceb_pdf_free`; input is limited to 1 GiB. No function writes files.
- UVZ input bytes are borrowed until `qw_uvz_document_close`. The page list keeps
  only `.pdg`/`.page` members; SSReader metadata (`Bookinfo.dat`,
  `BookContents.dat`, `ssct.data`) is ignored. A zero-length member is reported
  as `is_blank` and `qw_uvz_document_read_page` returns `QW_UVZ_OK` with a NULL
  buffer and size `0`.
- CAJ input bytes are borrowed until close; figure payloads and page text point
  into that input. The page API supports the `0xC8` and `HN` variants; the
  classic `CAJ` header reports `QW_CAJ_ERR_UNSUPPORTED`, but classic PDF
  extraction and outline parsing still work on it.
- CAJ figure kinds are metadata only (private JBIG, raster, bottom-up raster,
  JBIG2, JPEG 2000); this package does not decode them.
- Nothing is allocated behind the caller's back; entry points take memory only.

## Testing

`ctest` covers DjVu decoding against committed page references, IFF/BZZ/JB2/IW44
unit tests, CEB validation, the UVZ parser fixture (front matter, body pages, a
`.page` alias, metadata exclusion) and a deterministic fuzz smoke.

## Documentation

See [docs/usage.md](docs/usage.md) for container workflows, ownership rules and
format-specific caveats.

## License

MIT. See [LICENSE](LICENSE).

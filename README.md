# quirewise-page

Standalone Quirewise page library.

CMake target: `Quirewise::Page`.


Build with `cmake -S . -B build` and `cmake --build build`.

## Capabilities

- Opens SSReader UVZ containers: a ZIP whose members are page images named
  `NNNNNN.pdg` (the legacy `.page` alias is accepted too) inside an optional
  directory prefix. Page order follows the SSReader naming convention: cover,
  title page, front matter (`fow`/`pre`), table of contents (`!`), body pages,
  attachments, back cover.
- Other formats live in this package as well (PDF via the document package,
  DjVu, CEB) and follow the same page API.
- `qw_format_uvz` is exported for consumers that resolve formats themselves;
  like the CEB and DjVu descriptors it is not part of the shared registry, so a
  standalone page install does not depend on the application.

## Tests

`ctest` covers the page fuzz smoke, DjVu decoding, CEB validation and the UVZ
parser (container fixture with front matter, body pages, a `.page` alias,
metadata members that must stay out of the page list, and the format probe).
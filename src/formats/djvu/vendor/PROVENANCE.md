# Vendored DjVu decoder provenance

- Component: `djvudec` — plain-C DjVu decoder (port of DjvuNet).
- Upstream: https://github.com/kjk/djvudec
- Commit imported: `f0412eea9fe2f8d633104aab99b0e009b586b181` (2026-11-05; see upstream `PROGRESS.md`).
- License: MIT (see `LICENSE.djvudec.md` in this directory).
- Files: the unmodified `src/*.c` and `src/djvu.h`/`src/djvu_internal.h` from
  upstream; our wrapper is `../djvu_api.c` and does not edit the vendor code.
- Modifications: none. The amalgamated `dist/` drop is not used; the individual
  translation units are compiled directly.
- Why vendored: DjVu needs a full IW44/JB2/BZZ decoder; the upstream project is
  MIT, plain C, dependency-free and verified byte-for-byte against DjVuLibre.
  No GPL code (DjVuLibre) is included or linked.
- Consumers: Page package (`quirewise_page`), and PDG delegates DjVu probing,
  page count and rendering to the Page public API.

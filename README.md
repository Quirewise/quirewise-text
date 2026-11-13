# quirewise-text

Standalone C11 text and lightweight-markup readers.

CMake target: `Quirewise::Text`. Public headers are under `include/quirewise/`.

## Capabilities

- Markdown: renders the supported CommonMark subset to an HTML fragment
  (headings, paragraphs, emphasis, links, quotes, lists, rules, fenced and
  indented code).
- RTF: parses control words, groups and ANSI/Unicode escapes into an HTML
  fragment (paragraphs, bold/italic/underline/strike, size, alignment, page
  breaks, embedded PNG/JPEG pictures).
- Plain text: BOM/UTF-8 detection and paragraph layout helpers.
- HTML table visitor and grid helpers for HTML-to-table extraction.
- Bounded readers for AsciiDoc, LaTeX, Man, MediaWiki, Org, RST, Textile and
  Typst source, exposed through their own headers.

## Limits

The package is a **reader**: there are no writers for Markdown/HTML/EPUB/DOCX/
HWPX or any other document target (historical documentation listed such
outputs; they are not part of the public API). All markup support is a bounded
subset, not full CommonMark/RTF. Input size and output fragment size are
capped and report `RESOURCE_LIMIT`.

## Build

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

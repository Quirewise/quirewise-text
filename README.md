# quirewise-text

Readers for plain text, Markdown, RTF, roff man pages and seven lightweight
markup languages, plus an HTML table extractor.

`Quirewise::Text` is an independent, MIT-licensed C11 library. It is installed
as a CMake package and can be consumed without the Quirewise application or any
other Quirewise package.

## What it does

- Renders a supported Markdown subset to an HTML fragment: headings,
  paragraphs, emphasis/strong, code spans and fenced or indented code, links,
  blockquotes, ordered/unordered lists, horizontal rules and images (replaced
  by their alt text).
- Parses RTF into a complete HTML document: control words and groups,
  ANSI/Unicode escapes, code pages, bold/italic/underline/strike, font size,
  alignment, page breaks and embedded PNG/JPEG pictures as `data:` URIs.
- Parses roff man pages into a complete HTML document.
- Converts AsciiDoc, LaTeX, MediaWiki, Org mode, reStructuredText, Textile and
  Typst source into Markdown.
- Wraps plain text in a complete HTML document, detecting UTF-8/UTF-16 byte
  order marks and falling back to GBK/CP936 for legacy input.
- Visits HTML tables and reports row/column/cell text through callbacks, for
  both single-table and multi-table documents.
- Converts an HTML fragment back to the supported Markdown subset with
  `qw_markdown_from_html`: headings, paragraphs, emphasis, links, lists,
  blockquotes, inline and fenced code, simple tables, and images reduced to
  their alt text. `qw_markdown_text_free` releases the result.

**This package is a reader with one text writer.** The HTML-to-Markdown writer
is the exception; there is no writer for EPUB, DOCX, HWPX, PDF or any other
binary document target, and the HTML table extractor is a strict grid parser,
not a general HTML parser or sanitizer. Markup support is a bounded subset of
each language, not a full CommonMark, RTF or roff implementation.

## Requirements

- C11 compiler and CMake 3.21 or newer.
- The `QuirewiseCharset` and `QuirewiseRuntime` CMake packages must be
  installed; the exported `QuirewiseText` package config finds them.
- CTest is used for the test suite.

## Build, test and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/prefix
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Shared builds are available with `-DQUIREWISE_BUILD_SHARED=ON`.

## Use it from CMake

```cmake
find_package(QuirewiseText CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Text)
```

## Quick start

Render Markdown to an HTML fragment:

```c
#include <quirewise/markdown.h>

#include <stdio.h>

int main(void)
{
    static const char markdown[] =
        "# Title\n\nSome *emphasis* and a [link](https://example.com).\n";
    QwMarkdownDocument document = {0};

    if (qw_markdown_render_html(markdown, sizeof(markdown) - 1u, &document) !=
        QW_MARKDOWN_OK) {
        fputs("Markdown render failed\n", stderr);
        return 1;
    }
    fwrite(document.html, 1u, document.html_size, stdout);

    qw_markdown_document_free(&document);
    return 0;
}
```

Use `qw_markdown_status_string` (or the equivalent per header) when you need a
printable reason for a failure.

## API overview

| Header | Output |
|---|---|
| `quirewise/markdown.h` | HTML fragment |
| `quirewise/rtf.h` | complete HTML document plus RTF summary flags |
| `quirewise/man.h` | complete HTML document plus skipped-macro flag |
| `quirewise/plain_text.h` | complete HTML document |
| `quirewise/asciidoc.h` | Markdown |
| `quirewise/latex.h` | Markdown plus optional title |
| `quirewise/mediawiki.h` | Markdown |
| `quirewise/org.h` | Markdown |
| `quirewise/rst.h` | Markdown |
| `quirewise/textile.h` | Markdown |
| `quirewise/typst.h` | Markdown |
| `quirewise/html_grid.h` | row/cell callbacks |

| Task | Functions |
|---|---|
| Markdown to HTML | `qw_markdown_render_html`, `qw_markdown_document_free`, `qw_markdown_status_string` |
| RTF to HTML | `qw_rtf_parse_html`, `qw_rtf_document_free`, `qw_rtf_status_string` |
| Man page to HTML | `qw_man_parse_html`, `qw_man_document_free`, `qw_man_status_string` |
| Plain text to HTML | `qw_plain_text_probe`, `qw_plain_text_parse`, `qw_plain_text_document_html`, `qw_plain_text_document_free` |
| AsciiDoc to Markdown | `qw_asciidoc_parse_markdown`, `qw_asciidoc_document_free`, `qw_asciidoc_status_string` |
| LaTeX to Markdown | `qw_latex_parse_markdown`, `qw_latex_document_free`, `qw_latex_status_string` |
| MediaWiki to Markdown | `qw_mediawiki_parse_markdown`, `qw_mediawiki_document_free`, `qw_mediawiki_status_string` |
| Org to Markdown | `qw_org_parse_markdown`, `qw_org_document_free`, `qw_org_status_string` |
| reStructuredText to Markdown | `qw_rst_parse_markdown`, `qw_rst_document_free`, `qw_rst_status_string` |
| Textile to Markdown | `qw_textile_parse_markdown`, `qw_textile_document_free`, `qw_textile_status_string` |
| Typst to Markdown | `qw_typst_parse_markdown`, `qw_typst_document_free`, `qw_typst_status_string` |
| HTML tables | `qw_html_grid_visit_single_table`, `qw_html_grid_visit_tables` |

Document readers that expose a document struct also declare
`qw_<name>_abi_version`; the plain-text and HTML-grid headers do not.

## Behavior and limits

- The parser documents (`QwMarkdownDocument`, `QwRtfDocument`, ...) are
  caller-owned plain structs. Zero-initialize one before the call, read its
  output fields only on success, and pass it to the matching `*_document_free`.
- Input bytes are borrowed for the call. Output buffers are allocated by the
  library and released by the free function; pointer fields stay valid until
  then.
- Markdown output is a fragment. RTF, man and plain-text output are complete
  HTML documents (`<!doctype html>` ... `</body></html>`).
- Empty input and `NULL` data are invalid (`*_ERR_INVALID_ARGUMENT`).
- Input and output caps: Markdown 256 MiB in / 512 MiB out; RTF 256 MiB in /
  512 MiB out; man 64 MiB in / 128 MiB out; other markup readers 64 MiB in /
  128 MiB Markdown out; plain text 256 MiB in; HTML grids 512 MiB in. Limit
  violations report `*_ERR_RESOURCE_LIMIT`, except RTF input above 256 MiB,
  which reports `QW_RTF_ERR_UNSUPPORTED`.
- Status enums are small positive codes for the document readers and negative
  codes for `html_grid`; compare against named constants, never raw integers.
- RTF headers, footers, footnotes, annotations and embedded objects are
  suppressed. `saw_header_footer` flags header/footer suppression,
  `saw_footnote` flags footnotes, and `saw_object` flags OLE objects. Only PNG
  and JPEG pictures are rendered.

## Testing

`ctest` runs a Markdown rendering test (headings, emphasis, links, lists,
empty-input rejection) and a deterministic fuzz smoke that exercises all twelve
readers with mutated inputs, checking that every call returns a defined status
without crashing.

## Documentation

See [docs/usage.md](docs/usage.md) for task-oriented recipes, the visitor model,
status values, ownership and per-format limits.

## License

MIT. See [LICENSE](LICENSE).

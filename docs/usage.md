# quirewise-text usage guide

Task-level guide to `Quirewise::Text`. Each reader has its own header and status
enum; most provide `*_status_string`, and the document readers also provide
`*_abi_version`. Plain text and the HTML grid are the exceptions.

## 1. Build and integrate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/prefix
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(QuirewiseText CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Text)
```

`QuirewiseTextConfig.cmake` calls `find_dependency` for `QuirewiseRuntime` and
`QuirewiseCharset`, so install those packages in a visible prefix. Add
`-DQUIREWISE_BUILD_SHARED=ON` for shared libraries.

## 2. Core concepts

### 2.1 One struct per parse

Each reader fills a caller-owned struct. The pattern is always the same:

```c
QwMarkdownDocument document = {0};
QwMarkdownStatus status = qw_markdown_render_html(bytes, size, &document);
if (status == QW_MARKDOWN_OK) {
    /* use document.html / document.html_size */
    qw_markdown_document_free(&document);
}
```

The parser zeroes the struct before filling it, so initializing it yourself is
optional but recommended. Free only after a successful call; the free function
also accepts an all-zero struct. Input bytes are borrowed and may be reused as
soon as the call returns. Every output field is owned by the struct and stays
valid until `*_document_free`.

### 2.2 Output kinds

| Reader | Result |
|---|---|
| Markdown | HTML fragment |
| RTF | complete HTML document plus `saw_table`, `saw_picture`, `rendered_picture`, `omitted_picture`, `saw_object`, `saw_header_footer`, `saw_footnote` |
| Man | complete HTML document plus `skipped_unsupported_macros` |
| Plain text | complete HTML document; text is wrapped in `<pre>` |
| AsciiDoc, LaTeX, MediaWiki, Org, RST, Textile, Typst | Markdown, plus per-reader counters such as `omitted_images`, `dropped_unsafe_links`, `reduced_math` |
| HTML grid | no document; callbacks receive rows and cells |

RTF, man and plain-text readers emit `<!doctype html>` wrappers in the current
implementation; Markdown emits only the body fragment. Do not nest the complete
documents in another page without extracting the body.

### 2.3 Statuses and counters

Document readers return small positive status enums with `OK == 0` and
`QW_<READER>_ERR_*` failure values. `qw_html_grid_*` instead uses the negative
`QwHtmlGridResult` scale; compare against named constants, never raw integers,
and use `*_status_string` for logs. Counters such as `removed_images`,
`omitted_picture` or `skipped_unsupported_macros` are informational: they
never turn a successful parse into a failure.

## 3. Workflows
Recipes are excerpts; place them inside a function and supply the inputs.

### 3.1 Markdown to an HTML fragment

```c
#include <quirewise/markdown.h>
#include <stdio.h>

QwMarkdownDocument document = {0};
if (qw_markdown_render_html(source, source_size, &document) != QW_MARKDOWN_OK) {
    fputs("invalid or unsupported Markdown\n", stderr);
    return 1;
}
printf("removed images: %zu\n", document.removed_images);
fwrite(document.html, 1u, document.html_size, stdout);
qw_markdown_document_free(&document);
```

`document.html` is NUL-terminated; `html_size` excludes the terminator. Empty
input is rejected rather than treated as an empty document.

### 3.2 AsciiDoc to Markdown to HTML

Markup converters produce Markdown, so a two-stage pipeline reuses the Markdown
renderer:

```c
QwAsciiDocDocument adoc = {0};
if (qw_asciidoc_parse_markdown(source, source_size, &adoc) == QW_ASCIIDOC_OK) {
    QwMarkdownDocument html = {0};
    if (qw_markdown_render_html(adoc.markdown, adoc.markdown_size, &html) ==
        QW_MARKDOWN_OK) {
        fwrite(html.html, 1u, html.html_size, stdout);
        qw_markdown_document_free(&html);
    }
    qw_asciidoc_document_free(&adoc);
}
```

The same pattern applies to `qw_latex_parse_markdown`,
`qw_mediawiki_parse_markdown`, `qw_org_parse_markdown`,
`qw_rst_parse_markdown`, `qw_textile_parse_markdown` and
`qw_typst_parse_markdown`. LaTeX additionally exposes an optional `title` span.
When linking AsciiDoc into a document system, check `dropped_unsafe_links` and
`omitted_images` to see what the bounded converter had to skip.

### 3.3 RTF to HTML with a summary

```c
QwRtfDocument document = {0};
QwRtfStatus status = qw_rtf_parse_html(rtf, rtf_size, &document);
if (status != QW_RTF_OK) {
    fprintf(stderr, "RTF: %s\n", qw_rtf_status_string(status));
    return 1;
}
fwrite(document.html, 1u, document.html_size, stdout);
if (document.omitted_picture)
    fputs("note: unsupported picture formats were dropped\n", stderr);
if (document.saw_header_footer || document.saw_footnote)
    fputs("note: headers/footnotes are not part of the output\n", stderr);
qw_rtf_document_free(&document);
```

Embedded PNG and JPEG pictures become `data:image/png;base64,...` or
`data:image/jpeg;base64,...` sources, so the output is self-contained.
`QW_RTF_ERR_ENCODING` means the ANSI code page could not be converted.

### 3.4 Extract an HTML table

The visitor receives zero-based indices and borrowed cell text. Returning a
nonzero status from any callback aborts the walk and is propagated:

```c
#include <quirewise/html_grid.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Collector {
    int rows;
    int cells;
} Collector;

static int begin_row(void *context, size_t row)
{
    (void)row;
    ++((Collector *)context)->rows;
    return 0;
}

static int cell(void *context, size_t row, size_t column,
                const uint8_t *text, size_t text_size)
{
    Collector *collector = (Collector *)context;
    (void)row;
    (void)column;
    ++collector->cells;
    printf("%.*s\n", (int)text_size, (const char *)text);
    return 0;
}

static int end_row(void *context, size_t row)
{
    (void)context;
    (void)row;
    return 0;
}

int main(void)
{
    static const char html[] =
        "<table><tr><td>A</td><td>B</td></tr>"
        "<tr><td>C</td><td>D</td></tr></table>";
    Collector collector = {0, 0};
    QwHtmlGridVisitor visitor = {begin_row, cell, end_row};
    int rc = qw_html_grid_visit_single_table(
        (const uint8_t *)html, sizeof(html) - 1u, &visitor, &collector);

    if (rc != QW_HTML_GRID_OK) {
        fprintf(stderr, "table walk failed: %d\n", rc);
        return 1;
    }
    printf("%d row(s), %d cell(s)\n", collector.rows, collector.cells);
    return 0;
}
```

`qw_html_grid_visit_tables` takes a `QwHtmlGridDocumentVisitor` whose
`begin_table`/`end_table` callbacks receive the table index and, when present,
the text of the preceding `<h2>` as the table name. The parser is strict: it
expects one table (single-table call), optional `<tbody>`, only grid tags, and
rejects anything else with `QW_HTML_GRID_ERROR_UNSUPPORTED` or
`QW_HTML_GRID_ERROR_FORMAT`. A `<th scope="row">` is treated as a row label and
is not delivered as a cell.

### 3.5 Plain text with encoding detection

```c
QwPlainTextDocument *document = NULL;
size_t html_size = 0u;
const uint8_t *html;

if (!qw_plain_text_probe(bytes, size)) {
    fputs("not plain text (binary, ZIP/PDF or control bytes)\n", stderr);
    return 1;
}
if (qw_plain_text_parse(bytes, size, &document) != QW_PLAIN_TEXT_OK) {
    fputs("plain-text parse failed\n", stderr);
    return 1;
}
html = qw_plain_text_document_html(document, &html_size);
if (html && html_size)
    fwrite(html, 1u, html_size, stdout);
qw_plain_text_document_free(document);
```

The reader skips a UTF-8 BOM, decodes UTF-16LE/BE when a BOM is present, and
falls back to GBK/CP936 when the bytes are neither valid UTF-8 nor UTF-16.
`qw_plain_text_probe` is a cheap pre-check: it rejects ZIP and PDF signatures,
NUL-containing binary input and forbidden control characters. It does not
validate that the content is meaningful prose.

## 4. Error handling

All document readers share the same shape:

| Family | Success | Extra failure codes beyond argument/OOM/resource |
|---|---|---|
| Markdown | `QW_MARKDOWN_OK` | `FORMAT`, `UNSUPPORTED` |
| RTF | `QW_RTF_OK` | `FORMAT`, `UNSUPPORTED`, `ENCODING` |
| Man | `QW_MAN_OK` | `INVALID_UTF8`, `FORMAT` |
| AsciiDoc / LaTeX / Typst | `QW_ASCIIDOC_OK`, `QW_LATEX_OK`, `QW_TYPST_OK` | `INVALID_UTF8` |
| MediaWiki / Org / RST / Textile | `QW_MEDIAWIKI_OK`, `QW_ORG_OK`, `QW_RST_OK`, `QW_TEXTILE_OK` | none (unsupported markup is skipped) |
| Plain text | `QW_PLAIN_TEXT_OK` | `FORMAT`, `UNSUPPORTED`, `DECODE`, `UNKNOWN` |

`QW_PLAIN_TEXT_ERROR_DECODE` means the bytes were neither valid UTF-8 nor
decodable as GBK. AsciiDoc, LaTeX, man and Typst report invalid UTF-8 through
their `INVALID_UTF8` code, Markdown reports `FORMAT`, and RTF code-page failures
use `ENCODING`. MediaWiki, Org, RST and Textile do not validate UTF-8.

HTML grids use negative results:

| Value | Meaning |
|---|---|
| `QW_HTML_GRID_OK` (0) | success |
| `QW_HTML_GRID_ERROR_INVALID_ARGUMENT` (-1) | null input, visitor or callback; empty input |
| `QW_HTML_GRID_ERROR_FORMAT` (-4) | malformed grid markup |
| `QW_HTML_GRID_ERROR_UNSUPPORTED` (-5) | valid HTML outside the supported grid subset |
| `QW_HTML_GRID_ERROR_RESOURCE_LIMIT` (-11) | input, cell or table limit exceeded |

A callback's nonzero return value is passed through unchanged, so use a value
that cannot collide with `QW_HTML_GRID_OK` when you abort early.

## 5. Memory, lifetime and threading

- Inputs are borrowed for the call. Each successful parse allocates its output
  buffer; free it with the matching `*_document_free`, which resets the struct.
- Callback text spans in `QwHtmlGridVisitor` are valid only during the
  callback; copy them if you need to keep them.
- There is no global mutable state and no initialization step. Separate
  documents may be parsed concurrently from different threads.
- The library never keeps a pointer to caller memory after a call returns.

## 6. Limits and format notes

| Reader | Input cap | Output cap |
|---|---|---|
| Markdown | 256 MiB | 512 MiB HTML |
| RTF | 256 MiB | 512 MiB HTML |
| Man | 64 MiB | 128 MiB HTML |
| AsciiDoc / LaTeX / MediaWiki / Org / RST / Textile / Typst | 64 MiB | 128 MiB Markdown |
| Plain text | 256 MiB | none stated |
| HTML grid | 512 MiB | one cell 256 MiB, name 4 KiB, 256 tables |

- Markdown supports a CommonMark-style subset: ATX headings, paragraphs,
  emphasis/strong, code spans, fenced and four-space indented code, inline
  links, images (alt text), blockquotes, ordered/unordered lists and rules. It
  has no table-block support; nesting deeper than 32 reports
  `QW_MARKDOWN_ERR_UNSUPPORTED`.
- RTF group nesting beyond 512 levels is rejected as malformed, and input above
  256 MiB reports `QW_RTF_ERR_UNSUPPORTED` rather than a resource limit.
  Headers, footers, footnotes, annotations and OLE objects are suppressed.
- Man support covers a bounded macro set: `TH`, `SH`, `SS`, `P`/`PP`/`LP`/`SP`,
  `TP`, `IP`, `B`/`I`/`SM`/`SB`, `BR`/`IR`/`BI`/`IB`/`RB`/`RI`, `NF`/`EX`,
  `FI`/`EE`, `RS`/`RE` and `DE`/`AM`/`IG` plus `.br`. Other macros are skipped
  and counted; `SO` and `MSO` reject the page with `QW_MAN_ERR_FORMAT`.
- The seven markup converters drop unsafe link targets and images according to
  their counters; they never fetch remote resources.
- The HTML grid parser requires UTF-8, rejects NUL bytes inside cells, and
  accepts only `html`, `head`, `body`, `meta`, `style`, `section`, `h2`,
  `table`, `tbody`, `tr`, `td` and `th`. CSS is skipped, not interpreted.

## 7. Related packages

- `Quirewise::Charset` converts RTF code pages and provides the GBK/CP936
  fallback for plain text; `Quirewise::Runtime` supplies shared status and
  utility definitions. Both are public dependencies.
- `Quirewise::Document` and `Quirewise::Ebook` consume `Quirewise::Text` for
  their markup and plain-text inputs.

## 8. License

MIT. See `LICENSE`.

#include <quirewise/asciidoc.h>
#include <quirewise/html_grid.h>
#include <quirewise/latex.h>
#include <quirewise/man.h>
#include <quirewise/markdown.h>
#include <quirewise/mediawiki.h>
#include <quirewise/org.h>
#include <quirewise/plain_text.h>
#include <quirewise/rst.h>
#include <quirewise/rtf.h>
#include <quirewise/textile.h>
#include <quirewise/typst.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_SEED UINT32_C(0x51f03388)
#define FUZZ_RUNS 200u

static uint32_t rng_state = FUZZ_SEED;

static uint32_t next_random(void)
{
    uint32_t value = rng_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    rng_state = value;
    return value;
}

static size_t mutate(const char *seed, char *buffer, size_t capacity)
{
    size_t length = strlen(seed);
    size_t index;
    unsigned int flips = 1u + next_random() % 4u;
    if (length + 1u > capacity) length = capacity - 1u;
    memcpy(buffer, seed, length);
    buffer[length] = '\0';
    if (next_random() % 8u == 0u && length > 1u) {
        length = 1u + next_random() % length;
    }
    for (index = 0u; index < flips && length; ++index) {
        size_t position = next_random() % length;
        buffer[position] = (char)(buffer[position] ^
                                  (uint8_t)(1u << (next_random() % 8u)));
    }
    return length;
}

static int grid_begin_row(void *context, size_t row_index)
{
    (void)context;
    (void)row_index;
    return 0;
}

static int grid_cell(void *context, size_t row_index, size_t column_index,
                     const uint8_t *text, size_t text_size)
{
    (void)context;
    (void)row_index;
    (void)column_index;
    (void)text;
    (void)text_size;
    return 0;
}

static int grid_end_row(void *context, size_t row_index)
{
    (void)context;
    (void)row_index;
    return 0;
}

typedef enum ParserKind {
    PARSER_ASCIIDOC,
    PARSER_LATEX,
    PARSER_MAN,
    PARSER_MARKDOWN,
    PARSER_MEDIAWIKI,
    PARSER_ORG,
    PARSER_RST,
    PARSER_TEXTILE,
    PARSER_TYPST,
    PARSER_PLAIN,
    PARSER_RTF,
    PARSER_GRID
} ParserKind;

typedef struct FuzzSeed {
    ParserKind kind;
    const char *text;
} FuzzSeed;

static int run_parser(ParserKind kind, const char *data, size_t size)
{
    switch (kind) {
    case PARSER_ASCIIDOC: {
        QwAsciiDocDocument document = {0};
        int status = qw_asciidoc_parse_markdown(data, size, &document);
        if (status == QW_ASCIIDOC_OK) qw_asciidoc_document_free(&document);
        return status;
    }
    case PARSER_LATEX: {
        QwLatexDocument document = {0};
        int status = qw_latex_parse_markdown(data, size, &document);
        if (status == QW_LATEX_OK) qw_latex_document_free(&document);
        return status;
    }
    case PARSER_MAN: {
        QwManDocument document = {0};
        int status = qw_man_parse_html(data, size, &document);
        if (status == QW_MAN_OK) qw_man_document_free(&document);
        return status;
    }
    case PARSER_MARKDOWN: {
        QwMarkdownDocument document = {0};
        int status = qw_markdown_render_html(data, size, &document);
        if (status == QW_MARKDOWN_OK) qw_markdown_document_free(&document);
        return status;
    }
    case PARSER_MEDIAWIKI: {
        QwMediaWikiDocument document = {0};
        int status = qw_mediawiki_parse_markdown(data, size, &document);
        if (status == QW_MEDIAWIKI_OK) qw_mediawiki_document_free(&document);
        return status;
    }
    case PARSER_ORG: {
        QwOrgDocument document = {0};
        int status = qw_org_parse_markdown(data, size, &document);
        if (status == QW_ORG_OK) qw_org_document_free(&document);
        return status;
    }
    case PARSER_RST: {
        QwRstDocument document = {0};
        int status = qw_rst_parse_markdown(data, size, &document);
        if (status == QW_RST_OK) qw_rst_document_free(&document);
        return status;
    }
    case PARSER_TEXTILE: {
        QwTextileDocument document = {0};
        int status = qw_textile_parse_markdown(data, size, &document);
        if (status == QW_TEXTILE_OK) qw_textile_document_free(&document);
        return status;
    }
    case PARSER_TYPST: {
        QwTypstDocument document = {0};
        int status = qw_typst_parse_markdown(data, size, &document);
        if (status == QW_TYPST_OK) qw_typst_document_free(&document);
        return status;
    }
    case PARSER_PLAIN: {
        QwPlainTextDocument *document = NULL;
        int status = qw_plain_text_parse(data, size, &document);
        if (status == QW_PLAIN_TEXT_OK && document)
            qw_plain_text_document_free(document);
        return status;
    }
    case PARSER_RTF: {
        QwRtfDocument document = {0};
        int status = qw_rtf_parse_html(data, size, &document);
        if (status == QW_RTF_OK) qw_rtf_document_free(&document);
        return status;
    }
    case PARSER_GRID: {
        QwHtmlGridVisitor visitor = {grid_begin_row, grid_cell, grid_end_row};
        return qw_html_grid_visit_single_table((const uint8_t *)data, size,
                                               &visitor, NULL);
    }
    default:
        return -999;
    }
}

int main(void)
{
    static const FuzzSeed seeds[] = {
        {PARSER_ASCIIDOC, "= Title\n\n* item\n"},
        {PARSER_LATEX, "\\section{Title}\nText.\n"},
        {PARSER_MAN, ".TH TEST 1\n.SH NAME\nconsumer\n"},
        {PARSER_MARKDOWN, "# Title\n\n**bold** and `code`\n"},
        {PARSER_MEDIAWIKI, "== Title ==\n* item\n"},
        {PARSER_ORG, "* Title\n** child\n"},
        {PARSER_RST, "Title\n=====\n\n- item\n"},
        {PARSER_TEXTILE, "h1. Title\n\n*item*\n"},
        {PARSER_TYPST, "= Title\n\nText.\n"},
        {PARSER_PLAIN, "plain text\n"},
        {PARSER_RTF, "{\\rtf1\\ansi plain\\par}"},
        {PARSER_GRID, "<table><tr><td>a</td></tr></table>"},
    };
    char buffer[2048];
    size_t index;
    size_t run;
    size_t ok = 0u;
    size_t rejected = 0u;
    for (index = 0u; index < sizeof(seeds) / sizeof(seeds[0]); ++index) {
        for (run = 0u; run < FUZZ_RUNS; ++run) {
            size_t size = mutate(seeds[index].text, buffer, sizeof(buffer));
            if (run_parser(seeds[index].kind, buffer, size) == 0) {
                ++ok;
            } else {
                ++rejected;
            }
        }
    }
    printf("Text fuzz smoke: %zu ok, %zu rejected, seed=0x%08x runs=%u\n", ok,
           rejected, FUZZ_SEED, FUZZ_RUNS);
    return 0;
}

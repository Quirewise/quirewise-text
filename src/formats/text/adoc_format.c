#ifndef QUIREWISE_STANDALONE_ASCIIDOC_PARSER
#include "../../core/format_registry.h"
#include "markdown_format.h"
#endif
#include "../../core/text_buffer.h"

#include <quirewise/asciidoc.h>
#include "quirewise/status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ADOC_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    ADOC_MAX_MARKDOWN_BYTES = 128u * 1024u * 1024u,
    ADOC_PROBE_BYTES = 65536u
};

typedef struct AdocLine {
    const uint8_t *data;
    size_t size;
} AdocLine;

typedef struct AdocParser {
    QwTextBuffer markdown;
    int error;
    int skipped_unsafe;
    int omitted_images;
    int skipped_directives;
    int skipped_passthrough;
} AdocParser;

static int adoc_append(AdocParser *parser, const char *text, size_t size)
{
    if (parser->markdown.size > ADOC_MAX_MARKDOWN_BYTES ||
        size > ADOC_MAX_MARKDOWN_BYTES - parser->markdown.size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&parser->markdown, text, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int adoc_literal(AdocParser *parser, const char *text)
{
    return adoc_append(parser, text, strlen(text));
}

static int adoc_read_line(const uint8_t *data, size_t size,
                          size_t *position, AdocLine *out_line)
{
    size_t start;
    size_t end;
    if (!data || !position || !out_line || *position >= size) return 0;
    start = *position;
    end = start;
    while (end < size && data[end] != '\r' && data[end] != '\n') ++end;
    out_line->data = data + start;
    out_line->size = end - start;
    if (end < size && data[end] == '\r') ++end;
    if (end < size && data[end] == '\n') ++end;
    *position = end;
    return 1;
}

static int adoc_utf8_valid(const uint8_t *data, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        uint8_t first = data[i];
        size_t count;
        if (first < 0x80u) {
            if (!first || (first < 0x20u && first != '\r' && first != '\n' &&
                           first != '\t')) return 0;
            count = 1u;
        } else if (first >= 0xc2u && first <= 0xdfu) count = 2u;
        else if (first >= 0xe0u && first <= 0xefu) count = 3u;
        else if (first >= 0xf0u && first <= 0xf4u) count = 4u;
        else return 0;
        if (count > size - i) return 0;
        for (size_t j = 1u; j < count; ++j)
            if ((data[i + j] & 0xc0u) != 0x80u) return 0;
        if ((first == 0xe0u && data[i + 1u] < 0xa0u) ||
            (first == 0xedu && data[i + 1u] >= 0xa0u) ||
            (first == 0xf0u && data[i + 1u] < 0x90u) ||
            (first == 0xf4u && data[i + 1u] >= 0x90u)) return 0;
        i += count;
    }
    return 1;
}

static int adoc_blank(const AdocLine *line)
{
    size_t i;
    for (i = 0u; i < line->size; ++i)
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    return 1;
}

static int adoc_equal(const uint8_t *text, size_t size, const char *literal)
{
    size_t i;
    size_t length = strlen(literal);
    if (size != length) return 0;
    for (i = 0u; i < size; ++i) {
        uint8_t actual = text[i];
        uint8_t wanted = (uint8_t)literal[i];
        if (actual >= 'A' && actual <= 'Z') actual = (uint8_t)(actual + 32u);
        if (actual != wanted) return 0;
    }
    return 1;
}

static size_t adoc_find(const uint8_t *text, size_t size,
                        const char *needle, size_t start)
{
    size_t needle_size = strlen(needle);
    size_t i;
    if (!needle_size || start > size || needle_size > size - start) return SIZE_MAX;
    for (i = start; i <= size - needle_size; ++i)
        if (memcmp(text + i, needle, needle_size) == 0) return i;
    return SIZE_MAX;
}

static int adoc_prefix(const uint8_t *text, size_t size, const char *prefix)
{
    size_t i;
    size_t length = strlen(prefix);
    if (size < length) return 0;
    for (i = 0u; i < length; ++i) {
        uint8_t actual = text[i];
        uint8_t wanted = (uint8_t)prefix[i];
        if (actual >= 'A' && actual <= 'Z') actual = (uint8_t)(actual + 32u);
        if (actual != wanted) return 0;
    }
    return 1;
}

static int adoc_url_safe(const uint8_t *url, size_t size)
{
    size_t i;
    if (!url || !size) return 0;
    for (i = 0u; i < size; ++i)
        if (url[i] <= 0x20u || url[i] == '"' || url[i] == '\'' ||
            url[i] == '<' || url[i] == '>') return 0;
    return adoc_prefix(url, size, "https://") ||
           adoc_prefix(url, size, "http://") ||
           adoc_prefix(url, size, "mailto:");
}

static int adoc_append_plain(AdocParser *parser,
                             const uint8_t *text, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        const char *escaped = NULL;
        switch (text[i]) {
        case '\\': escaped = "\\\\"; break;
        case '`': escaped = "\\`"; break;
        case '*': escaped = "\\*"; break;
        case '_': escaped = "\\_"; break;
        case '[': escaped = "\\["; break;
        case ']': escaped = "\\]"; break;
        case '#': escaped = "\\#"; break;
        case '~': escaped = "\\~"; break;
        case '&': escaped = "&amp;"; break;
        case '<': escaped = "&lt;"; break;
        case '>': escaped = "&gt;"; break;
        default: break;
        }
        if (escaped) {
            if (!adoc_literal(parser, escaped)) return 0;
        } else if (!adoc_append(parser, (const char *)text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

static int adoc_inline_plain(AdocParser *parser,
                             const uint8_t *text, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        if (i + 6u <= size && adoc_prefix(text + i, size - i, "image::")) {
            size_t open = adoc_find(text, size, "[", i + 7u);
            size_t end = open == SIZE_MAX ? SIZE_MAX : adoc_find(text, size, "]", open + 1u);
            if (end != SIZE_MAX) {
                if (!adoc_literal(parser, "[Image omitted]")) return 0;
                parser->omitted_images = 1;
                i = end + 1u;
                continue;
            }
        }
        if (i + 6u <= size && adoc_prefix(text + i, size - i, "image:")) {
            size_t open = adoc_find(text, size, "[", i + 6u);
            size_t end = open == SIZE_MAX ? SIZE_MAX : adoc_find(text, size, "]", open + 1u);
            if (end != SIZE_MAX) {
                if (!adoc_literal(parser, "[Image omitted]")) return 0;
                parser->omitted_images = 1;
                i = end + 1u;
                continue;
            }
        }
        if (i + 5u <= size && adoc_prefix(text + i, size - i, "link:")) {
            size_t open = adoc_find(text, size, "[", i + 5u);
            size_t end = open == SIZE_MAX ? SIZE_MAX : adoc_find(text, size, "]", open + 1u);
            size_t url_start = i + 5u;
            if (open != SIZE_MAX && end != SIZE_MAX && open > url_start) {
                const uint8_t *url = text + url_start;
                size_t url_size = open - url_start;
                const uint8_t *label = text + open + 1u;
                size_t label_size = end - open - 1u;
                if (adoc_url_safe(url, url_size)) {
                    if (!adoc_literal(parser, "[") ||
                        !adoc_append_plain(parser, label, label_size) ||
                        !adoc_literal(parser, "](<")) return 0;
                    if (!adoc_append(parser, (const char *)url, url_size) ||
                        !adoc_literal(parser, ">)")) return 0;
                } else {
                    parser->skipped_unsafe = 1;
                    if (!adoc_append_plain(parser, label, label_size)) return 0;
                }
                i = end + 1u;
                continue;
            }
        }
        if (adoc_prefix(text + i, size - i, "https://") ||
            adoc_prefix(text + i, size - i, "http://") ||
            adoc_prefix(text + i, size - i, "mailto:")) {
            size_t url_end = i;
            size_t url_size;
            size_t label_end = SIZE_MAX;
            const uint8_t *label;
            size_t label_size;
            while (url_end < size && text[url_end] > 0x20u &&
                   text[url_end] != '<' && text[url_end] != '>') ++url_end;
            url_size = url_end - i;
            if (url_end < size && text[url_end] == '[')
                label_end = adoc_find(text, size, "]", url_end + 1u);
            label = label_end == SIZE_MAX ? text + i : text + url_end + 1u;
            label_size = label_end == SIZE_MAX ? url_size : label_end - url_end - 1u;
            if (adoc_url_safe(text + i, url_size)) {
                if (!adoc_literal(parser, "[") ||
                    !adoc_append_plain(parser, label, label_size) ||
                    !adoc_literal(parser, "](<") ||
                    !adoc_append(parser, (const char *)text + i, url_size) ||
                    !adoc_literal(parser, ">)")) return 0;
                i = label_end == SIZE_MAX ? url_end : label_end + 1u;
                continue;
            }
        }
        if (i + 2u <= size && text[i] == '<' && text[i + 1u] == '<') {
            size_t end = adoc_find(text, size, ">>", i + 2u);
            if (end != SIZE_MAX) {
                size_t separator = adoc_find(text + i + 2u, end - i - 2u, ",", 0u);
                size_t label_start = separator == SIZE_MAX ? i + 2u : i + 2u + separator + 1u;
                size_t label_size = end - label_start;
                while (label_size && text[label_start] == ' ') {
                    ++label_start;
                    --label_size;
                }
                if (label_size) {
                    if (!adoc_append_plain(parser, text + label_start, label_size)) return 0;
                } else if (!adoc_append_plain(parser, text + i + 2u,
                                              end - i - 2u)) return 0;
                i = end + 2u;
                continue;
            }
        }
        if ((text[i] == '`' || text[i] == '+') && i + 1u < size) {
            uint8_t marker = text[i];
            size_t end = i + 1u;
            while (end < size && text[end] != marker) ++end;
            if (end > i + 1u && end < size) {
                if (!adoc_literal(parser, "`") ||
                    !adoc_append_plain(parser, text + i + 1u, end - i - 1u) ||
                    !adoc_literal(parser, "`")) return 0;
                i = end + 1u;
                continue;
            }
        }
        if ((text[i] == '*' || text[i] == '_') && i + 1u < size &&
            (i == 0u || text[i - 1u] <= 0x20u || strchr("([{", text[i - 1u]))) {
            uint8_t marker = text[i];
            size_t end = i + 1u;
            while (end < size && text[end] != marker) ++end;
            if (end > i + 1u && end < size && text[end - 1u] > 0x20u) {
                const char *open = marker == '*' ? "**" : "*";
                if (!adoc_literal(parser, open) ||
                    !adoc_append_plain(parser, text + i + 1u, end - i - 1u) ||
                    !adoc_literal(parser, open)) return 0;
                i = end + 1u;
                continue;
            }
        }
        if (!adoc_append_plain(parser, text + i, 1u)) return 0;
        ++i;
    }
    return 1;
}

static int adoc_heading(const uint8_t *text, size_t size,
                        size_t *out_level, size_t *out_content)
{
    size_t markers = 0u;
    while (markers < size && text[markers] == '=') ++markers;
    if (!markers || markers > 6u || markers >= size || text[markers] != ' ')
        return 0;
    *out_level = markers;
    *out_content = markers;
    while (*out_content < size && text[*out_content] == ' ') ++*out_content;
    return 1;
}

static int adoc_list(const uint8_t *text, size_t size,
                     size_t *out_depth, int *out_ordered, size_t *out_content)
{
    uint8_t marker;
    size_t depth = 0u;
    if (!size || (text[0] != '*' && text[0] != '.')) return 0;
    marker = text[0];
    while (depth < size && text[depth] == marker) ++depth;
    if (depth > 6u || depth >= size || text[depth] != ' ') return 0;
    *out_depth = depth;
    *out_ordered = marker == '.';
    *out_content = depth + 1u;
    return 1;
}

static size_t adoc_max_backticks(const uint8_t *text, size_t size)
{
    size_t maximum = 0u;
    size_t run = 0u;
    size_t i;
    for (i = 0u; i < size; ++i) {
        if (text[i] == '`') {
            if (++run > maximum) maximum = run;
        } else run = 0u;
    }
    return maximum;
}

static int adoc_code_block(AdocParser *parser, const uint8_t *data, size_t size,
                           size_t *position, const char *delimiter)
{
    QwTextBuffer code = {0};
    size_t delimiter_size = strlen(delimiter);
    size_t fence;
    int ok = 1;
    code.limit = ADOC_MAX_MARKDOWN_BYTES + 1u;
    while (*position < size) {
        AdocLine line;
        if (!adoc_read_line(data, size, position, &line)) break;
        if (line.size == delimiter_size &&
            memcmp(line.data, delimiter, delimiter_size) == 0) break;
        if (!qw_text_buffer_append(&code, (const char *)line.data, line.size) ||
            !qw_text_buffer_append(&code, "\n", 1u)) {
            parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
            ok = 0;
            break;
        }
    }
    if (ok) {
        fence = adoc_max_backticks((const uint8_t *)code.data, code.size) + 1u;
        if (fence < 3u) fence = 3u;
        for (size_t i = 0u; i < fence; ++i)
            if (!adoc_literal(parser, "`") ) { ok = 0; break; }
        if (ok && (!adoc_literal(parser, "\n") ||
                   (code.size && !adoc_append(parser, code.data, code.size)))) ok = 0;
        for (size_t i = 0u; ok && i < fence; ++i)
            if (!adoc_literal(parser, "`")) ok = 0;
        if (ok && !adoc_literal(parser, "\n\n")) ok = 0;
        if (!ok && !parser->error) parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    free(code.data);
    return ok;
}

static int adoc_skip_block(const uint8_t *data, size_t size,
                           size_t *position, const char *delimiter)
{
    size_t delimiter_size = strlen(delimiter);
    while (*position < size) {
        AdocLine line;
        if (!adoc_read_line(data, size, position, &line)) break;
        if (line.size == delimiter_size &&
            memcmp(line.data, delimiter, delimiter_size) == 0) return 1;
    }
    return 0;
}

static int adoc_conditional_block(const AdocLine *line)
{
    return line && line->size >= 2u &&
           line->data[line->size - 2u] == '[' &&
           line->data[line->size - 1u] == ']';
}

static void adoc_skip_conditional(const uint8_t *data, size_t size,
                                 size_t *position)
{
    size_t depth = 1u;
    while (*position < size) {
        AdocLine line;
        if (!adoc_read_line(data, size, position, &line)) break;
        if ((adoc_prefix(line.data, line.size, "ifdef::") ||
             adoc_prefix(line.data, line.size, "ifndef::")) &&
            adoc_conditional_block(&line)) {
            ++depth;
        } else if (adoc_prefix(line.data, line.size, "endif::")) {
            if (--depth == 0u) return;
        }
    }
}

static int adoc_translate(const uint8_t *data, size_t size, AdocParser *parser)
{
    size_t position = 0u;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    while (position < size) {
        AdocLine line;
        size_t heading_level;
        size_t heading_content;
        size_t list_depth;
        size_t list_content;
        int ordered;
        if (!adoc_read_line(data, size, &position, &line)) break;
        if (adoc_blank(&line)) {
            if (!adoc_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.size == 4u && memcmp(line.data, "////", 4u) == 0) {
            (void)adoc_skip_block(data, size, &position, "////");
            parser->skipped_passthrough = 1;
            if (!adoc_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.size == 4u && memcmp(line.data, "++++", 4u) == 0) {
            (void)adoc_skip_block(data, size, &position, "++++");
            parser->skipped_passthrough = 1;
            if (!adoc_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.size == 4u && memcmp(line.data, "----", 4u) == 0) {
            if (!adoc_code_block(parser, data, size, &position, "----")) return 0;
            continue;
        }
        if (line.size == 4u && memcmp(line.data, "....", 4u) == 0) {
            if (!adoc_code_block(parser, data, size, &position, "....")) return 0;
            continue;
        }
        if (line.size == 4u && memcmp(line.data, "____", 4u) == 0) {
            int closed = 0;
            if (!adoc_literal(parser, "\n")) return 0;
            while (position < size) {
                AdocLine quote_line;
                if (!adoc_read_line(data, size, &position, &quote_line)) break;
                if (quote_line.size == 4u &&
                    memcmp(quote_line.data, "____", 4u) == 0) {
                    closed = 1;
                    break;
                }
                if (!adoc_literal(parser, "> ") ||
                    !adoc_inline_plain(parser, quote_line.data, quote_line.size) ||
                    !adoc_literal(parser, "\n")) return 0;
            }
            if (!closed) parser->skipped_directives = 1;
            if (!adoc_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.size == 4u && (memcmp(line.data, "====", 4u) == 0 ||
                                memcmp(line.data, "****", 4u) == 0)) {
            if (!adoc_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.size == 2u && memcmp(line.data, "--", 2u) == 0) {
            if (!adoc_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.size >= 7u && (adoc_prefix(line.data, line.size, "ifdef::") ||
                                adoc_prefix(line.data, line.size, "ifndef::"))) {
            if (adoc_conditional_block(&line))
                adoc_skip_conditional(data, size, &position);
            parser->skipped_directives = 1;
            continue;
        }
        if ((line.size >= 2u && line.data[0] == '/' && line.data[1] == '/') ||
            (line.size >= 9u && adoc_prefix(line.data, line.size, "include::")) ||
            (line.size >= 7u && adoc_prefix(line.data, line.size, "endif::"))) {
            parser->skipped_directives = 1;
            continue;
        }
        if (line.size >= 3u && line.data[0] == ':' &&
            memchr(line.data + 1u, ':', line.size - 1u) != NULL) continue;
        if (line.size >= 8u && line.data[0] == '[' && line.data[line.size - 1u] == ']' &&
            (adoc_prefix(line.data, line.size, "[source") ||
             adoc_prefix(line.data, line.size, "[listing") ||
             adoc_prefix(line.data, line.size, "[literal"))) continue;
        if (adoc_heading(line.data, line.size, &heading_level, &heading_content)) {
            if (!adoc_literal(parser, "\n")) return 0;
            for (size_t i = 0u; i < heading_level; ++i)
                if (!adoc_literal(parser, "#")) return 0;
            if (!adoc_literal(parser, " ") ||
                !adoc_inline_plain(parser, line.data + heading_content,
                                   line.size - heading_content) ||
                !adoc_literal(parser, "\n\n")) return 0;
            continue;
        }
        if (adoc_list(line.data, line.size, &list_depth, &ordered, &list_content)) {
            for (size_t i = 1u; i < list_depth; ++i)
                if (!adoc_literal(parser, "  ")) return 0;
            if (!adoc_literal(parser, ordered ? "1. " : "- ") ||
                !adoc_inline_plain(parser, line.data + list_content,
                                   line.size - list_content) ||
                !adoc_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.size >= 7u && (adoc_prefix(line.data, line.size, "image::") ||
                                adoc_prefix(line.data, line.size, "image:"))) {
            if (!adoc_literal(parser, "[Image omitted]\n\n")) return 0;
            parser->omitted_images = 1;
            continue;
        }
        if (line.size >= 6u && (adoc_prefix(line.data, line.size, "NOTE: ") ||
                                adoc_prefix(line.data, line.size, "TIP: ") ||
                                adoc_prefix(line.data, line.size, "WARNING: ") ||
                                adoc_prefix(line.data, line.size, "IMPORTANT: ") ||
                                adoc_prefix(line.data, line.size, "CAUTION: "))) {
            size_t colon = 0u;
            while (colon < line.size && line.data[colon] != ':') ++colon;
            if (!adoc_literal(parser, "> **") ||
                !adoc_append_plain(parser, line.data, colon) ||
                !adoc_literal(parser, ":** ") ||
                !adoc_inline_plain(parser, line.data + colon + 2u,
                                   line.size - colon - 2u) ||
                !adoc_literal(parser, "\n\n")) return 0;
            continue;
        }
        {
            size_t separator = adoc_find(line.data, line.size, "::", 0u);
            if (separator != SIZE_MAX && separator > 0u && separator + 2u < line.size) {
                if (!adoc_literal(parser, "**") ||
                    !adoc_inline_plain(parser, line.data, separator) ||
                    !adoc_literal(parser, "** — ") ||
                    !adoc_inline_plain(parser, line.data + separator + 2u,
                                       line.size - separator - 2u) ||
                    !adoc_literal(parser, "\n\n")) return 0;
                continue;
            }
        }
        if (line.size && line.data[0] == '.') {
            size_t i;
            for (i = 1u; i < line.size && line.data[i] != ' '; ++i) { }
            if (i == line.size) continue;
        }
        if (!adoc_inline_plain(parser, line.data, line.size) ||
            !adoc_literal(parser, "\n\n")) return 0;
    }
    return 1;
}

int qw_asciidoc_abi_version(void)
{
    return QW_ASCIIDOC_ABI_VERSION;
}

QwAsciiDocStatus qw_asciidoc_parse_markdown(const void *data, size_t size,
                                           QwAsciiDocDocument *out_document)
{
    AdocParser parser;
    if (!data || !size || !out_document)
        return QW_ASCIIDOC_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (size > ADOC_MAX_INPUT_BYTES) return QW_ASCIIDOC_ERR_RESOURCE_LIMIT;
    if (!adoc_utf8_valid((const uint8_t *)data, size))
        return QW_ASCIIDOC_ERR_INVALID_UTF8;
    memset(&parser, 0, sizeof(parser));
    parser.markdown.limit = ADOC_MAX_MARKDOWN_BYTES + 1u;
    if (!adoc_translate((const uint8_t *)data, size, &parser)) {
        QwAsciiDocStatus status = parser.error == QW_STATUS_ERR_RESOURCE_LIMIT
            ? QW_ASCIIDOC_ERR_RESOURCE_LIMIT : QW_ASCIIDOC_ERR_OUT_OF_MEMORY;
        free(parser.markdown.data);
        return status;
    }
    out_document->markdown = parser.markdown.data;
    out_document->markdown_size = parser.markdown.size;
    out_document->omitted_images = parser.omitted_images;
    out_document->skipped_directives = parser.skipped_directives;
    out_document->skipped_passthrough = parser.skipped_passthrough;
    out_document->dropped_unsafe_links = parser.skipped_unsafe;
    return QW_ASCIIDOC_OK;
}

void qw_asciidoc_document_free(QwAsciiDocDocument *document)
{
    if (!document) return;
    free(document->markdown);
    memset(document, 0, sizeof(*document));
}

const char *qw_asciidoc_status_string(QwAsciiDocStatus status)
{
    switch (status) {
    case QW_ASCIIDOC_OK: return "success";
    case QW_ASCIIDOC_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_ASCIIDOC_ERR_INVALID_UTF8: return "input is not valid UTF-8";
    case QW_ASCIIDOC_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_ASCIIDOC_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    default: return "unknown AsciiDoc parser status";
    }
}

#ifndef QUIREWISE_STANDALONE_ASCIIDOC_PARSER

static int adoc_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t position = 0u;
    size_t scan_size;
    if (!input || !size || size > ADOC_MAX_INPUT_BYTES) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    scan_size = size < ADOC_PROBE_BYTES ? size : ADOC_PROBE_BYTES;
    if (!adoc_utf8_valid(data + position, scan_size - position)) return 0;
    while (position < scan_size) {
        AdocLine line;
        size_t level;
        size_t content;
        if (!adoc_read_line(data, scan_size, &position, &line)) break;
        if (adoc_heading(line.data, line.size, &level, &content) ||
            adoc_prefix(line.data, line.size, "[source,") ||
            adoc_prefix(line.data, line.size, ":doctype:") ||
            adoc_prefix(line.data, line.size, "image::") ||
            adoc_prefix(line.data, line.size, "include::")) return 1;
    }
    return 0;
}

static int adoc_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    if (!dot) return 0;
    return adoc_equal((const uint8_t *)dot, strlen(dot), ".adoc") ||
           adoc_equal((const uint8_t *)dot, strlen(dot), ".asciidoc") ||
           adoc_equal((const uint8_t *)dot, strlen(dot), ".asc");
}

static int adoc_open_layout(const void *input, size_t input_size,
                            QwInputFormat input_format,
                            const QwConvertOptions *options,
                            QwLayoutDocument *out_document,
                            QwConvertReport *report)
{
    QwAsciiDocDocument adoc = {0};
    int omitted_images;
    int skipped_directives;
    int skipped_passthrough;
    int dropped_unsafe_links;
    int rc;
    (void)options;
    if (!input || !input_size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_ADOC) return QW_STATUS_ERR_INVALID_ARGUMENT;
    {
        QwAsciiDocStatus status = qw_asciidoc_parse_markdown(
            input, input_size, &adoc);
        if (status == QW_ASCIIDOC_ERR_INVALID_ARGUMENT)
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        if (status == QW_ASCIIDOC_ERR_INVALID_UTF8) return QW_STATUS_ERR_DECODE;
        if (status == QW_ASCIIDOC_ERR_RESOURCE_LIMIT)
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        if (status != QW_ASCIIDOC_OK) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    omitted_images = adoc.omitted_images;
    skipped_directives = adoc.skipped_directives;
    skipped_passthrough = adoc.skipped_passthrough;
    dropped_unsafe_links = adoc.dropped_unsafe_links;
    rc = qw_markdown_format_open_layout(adoc.markdown,
                                        adoc.markdown_size,
                                        QW_FORMAT_MARKDOWN, NULL,
                                        out_document, report);
    qw_asciidoc_document_free(&adoc);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        (void)snprintf(report->note, sizeof(report->note),
            "AsciiDoc subset rendered through CommonMark; include/conditional directives and passthrough blocks are not executed%s%s%s",
            omitted_images ? "; images were replaced by text" : "",
            skipped_directives ? "; document directives were skipped" : "",
            dropped_unsafe_links ? "; unsafe links were reduced to label text" :
                (skipped_passthrough ? "; passthrough content was omitted" : ""));
    }
    return rc;
}

const QwFormatDescriptor qw_format_adoc = {
    .id = QW_FORMAT_ADOC,
    .name = "AsciiDoc",
    .probe = adoc_probe,
    .open_layout = adoc_open_layout,
    .max_input_bytes = ADOC_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};

const QwPathFormatDescriptor qw_path_format_adoc = {
    .id = QW_FORMAT_ADOC,
    .probe_path = adoc_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};

#endif

#ifndef QUIREWISE_STANDALONE_ORG_PARSER
#include "../../core/format_registry.h"
#include "markdown_format.h"
#endif
#include "../../core/text_buffer.h"

#include <quirewise/org.h>
#include "quirewise/status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ORG_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    ORG_MAX_MARKDOWN_BYTES = 128u * 1024u * 1024u,
    ORG_PROBE_BYTES = 65536u
};

typedef struct OrgLine {
    const uint8_t *data;
    size_t size;
} OrgLine;

typedef struct OrgParser {
    QwTextBuffer markdown;
    int error;
    int dropped_link;
    int omitted_export;
} OrgParser;

static int org_append(OrgParser *parser, const char *text, size_t size)
{
    if (parser->markdown.size > ORG_MAX_MARKDOWN_BYTES ||
        size > ORG_MAX_MARKDOWN_BYTES - parser->markdown.size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&parser->markdown, text, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int org_literal(OrgParser *parser, const char *text)
{
    return org_append(parser, text, strlen(text));
}

static int org_repeated(OrgParser *parser, char value, size_t count)
{
    char chunk[32];
    memset(chunk, value, sizeof(chunk));
    while (count) {
        size_t amount = count < sizeof(chunk) ? count : sizeof(chunk);
        if (!org_append(parser, chunk, amount)) return 0;
        count -= amount;
    }
    return 1;
}

static int org_read_line(const uint8_t *data, size_t size,
                         size_t *position, OrgLine *out_line)
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

static uint8_t org_ascii_lower(uint8_t value)
{
    return value >= 'A' && value <= 'Z'
        ? (uint8_t)(value + ('a' - 'A')) : value;
}

static int org_equal(const uint8_t *text, size_t size, const char *literal)
{
    size_t i;
    size_t length = strlen(literal);
    if (size != length) return 0;
    for (i = 0u; i < size; ++i)
        if (org_ascii_lower(text[i]) != (uint8_t)literal[i]) return 0;
    return 1;
}

static int org_prefix(const uint8_t *text, size_t size, const char *literal,
                      size_t *out_length)
{
    size_t i;
    size_t length = strlen(literal);
    if (size < length) return 0;
    for (i = 0u; i < length; ++i)
        if (org_ascii_lower(text[i]) != (uint8_t)literal[i]) return 0;
    if (out_length) *out_length = length;
    return 1;
}

static int org_contains_pair(const uint8_t *text, size_t size,
                             uint8_t first, uint8_t second)
{
    size_t i;
    if (!text || size < 2u) return 0;
    for (i = 0u; i + 1u < size; ++i)
        if (text[i] == first && text[i + 1u] == second) return 1;
    return 0;
}

static int org_directive(const OrgLine *line, const char *name,
                         size_t *out_argument)
{
    size_t indent = 0u;
    size_t length;
    while (indent < line->size &&
           (line->data[indent] == ' ' || line->data[indent] == '\t')) ++indent;
    if (!org_prefix(line->data + indent, line->size - indent, name, &length))
        return 0;
    if (line->size > indent + length &&
        line->data[indent + length] != ' ' &&
        line->data[indent + length] != '\t') return 0;
    if (out_argument) {
        size_t argument = indent + length;
        while (argument < line->size &&
               (line->data[argument] == ' ' || line->data[argument] == '\t'))
            ++argument;
        *out_argument = argument;
    }
    return 1;
}

static int org_blank(const OrgLine *line)
{
    size_t i;
    for (i = 0u; i < line->size; ++i)
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    return 1;
}

static int org_url_safe(const uint8_t *url, size_t size)
{
    size_t i;
    int scheme = (size >= 7u && memcmp(url, "http://", 7u) == 0) ||
                 (size >= 8u && memcmp(url, "https://", 8u) == 0) ||
                 (size >= 7u && memcmp(url, "mailto:", 7u) == 0);
    if (!scheme) return 0;
    for (i = 0u; i < size; ++i)
        if (url[i] <= 0x20u || url[i] == 0x7fu || url[i] == '<' ||
            url[i] == '>' || url[i] == '"' || url[i] == '\\') return 0;
    return 1;
}

static int org_append_plain(OrgParser *parser, const uint8_t *text, size_t size)
{
    size_t i;
    size_t span = 0u;
    for (i = 0u; i < size; ++i) {
        const char *escaped = NULL;
        switch (text[i]) {
        case '&': escaped = "&amp;"; break;
        case '<': escaped = "&lt;"; break;
        case '>': escaped = "&gt;"; break;
        case '\\': escaped = "\\\\"; break;
        case '`': escaped = "\\`"; break;
        case '*': escaped = "\\*"; break;
        case '_': escaped = "\\_"; break;
        case '[': escaped = "\\["; break;
        case ']': escaped = "\\]"; break;
        case '#': escaped = "\\#"; break;
        case '~': escaped = "\\~"; break;
        default: break;
        }
        if (escaped) {
            if ((i > span && !org_append(parser,
                    (const char *)text + span, i - span)) ||
                !org_literal(parser, escaped)) return 0;
            span = i + 1u;
        }
    }
    return span == size || org_append(parser,
        (const char *)text + span, size - span);
}

static size_t org_max_backticks(const uint8_t *text, size_t size)
{
    size_t maximum = 0u;
    size_t current = 0u;
    size_t i;
    for (i = 0u; i < size; ++i) {
        if (text[i] == '`') {
            ++current;
            if (current > maximum) maximum = current;
        } else {
            current = 0u;
        }
    }
    return maximum;
}

static int org_append_code(OrgParser *parser, const uint8_t *text, size_t size)
{
    size_t fence = org_max_backticks(text, size) + 1u;
    if (!fence) fence = 1u;
    if (!org_repeated(parser, '`', fence)) return 0;
    if (size && (text[0] == ' ' || text[0] == '`') &&
        (text[size - 1u] == ' ' || text[size - 1u] == '`')) {
        if (!org_literal(parser, " ") ||
            !org_append(parser, (const char *)text, size) ||
            !org_literal(parser, " ")) return 0;
    } else if (!org_append(parser, (const char *)text, size)) {
        return 0;
    }
    return org_repeated(parser, '`', fence);
}

static int org_inline_delimited(OrgParser *parser, const char *open,
                                const char *close, const uint8_t *text,
                                size_t size)
{
    return org_literal(parser, open) && org_append_plain(parser, text, size) &&
           org_literal(parser, close);
}

static int org_inline(OrgParser *parser, const uint8_t *text, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        if (text[i] == '\\' && i + 1u < size && text[i + 1u] &&
            strchr("\\*_+/=~[]<>#|&", text[i + 1u])) {
            if (!org_append_plain(parser, text + i + 1u, 1u)) return 0;
            i += 2u;
            continue;
        }
        if (i + 1u < size && text[i] == '[' && text[i + 1u] == '[') {
            size_t close = i + 2u;
            size_t label = SIZE_MAX;
            while (close + 1u < size &&
                   !(text[close] == ']' && text[close + 1u] == ']')) {
                if (label == SIZE_MAX && text[close] == ']' &&
                    text[close + 1u] == '[') label = close;
                ++close;
            }
            if (close + 1u < size) {
                size_t url_size;
                const uint8_t *url = text + i + 2u;
                size_t label_start = label == SIZE_MAX ? i + 2u : label + 2u;
                size_t label_size = label == SIZE_MAX
                    ? close - i - 2u : close - label_start;
                url_size = label == SIZE_MAX
                    ? close - i - 2u : label - i - 2u;
                if (label == SIZE_MAX) {
                    label = close;
                    label_size = url_size;
                }
                if (org_url_safe(url, url_size)) {
                    if (!org_literal(parser, "[") ||
                        !org_append_plain(parser, text + label_start,
                                          label_size) ||
                        !org_literal(parser, "](<") ||
                        !org_append(parser, (const char *)url, url_size) ||
                        !org_literal(parser, ">)")) return 0;
                } else {
                    parser->dropped_link = 1;
                    if (!org_append_plain(parser, text + label_start,
                                          label_size)) return 0;
                }
                i = close + 2u;
                continue;
            }
            if (!org_append_plain(parser, text + i, size - i)) return 0;
            return 1;
        }
        if (text[i] == '=' || text[i] == '~') {
            uint8_t marker = text[i];
            size_t end = i + 1u;
            while (end < size && text[end] != marker) ++end;
            if (end < size && end > i + 1u) {
                if (!org_append_code(parser, text + i + 1u, end - i - 1u))
                    return 0;
                i = end + 1u;
                continue;
            }
        }
        if (text[i] == '*' || text[i] == '/') {
            uint8_t marker = text[i];
            size_t end = i + 1u;
            while (end < size && text[end] != marker) ++end;
            if (end < size && end > i + 1u && text[i + 1u] != ' ' &&
                text[end - 1u] != ' ' &&
                (i == 0u || text[i - 1u] <= 0x20u ||
                 strchr("([{\"'", text[i - 1u]) != NULL) &&
                (end + 1u == size || text[end + 1u] <= 0x20u ||
                 strchr(")]},.;:!?\"'", text[end + 1u]) != NULL)) {
                if (marker == '*') {
                    if (!org_inline_delimited(parser, "**", "**",
                        text + i + 1u, end - i - 1u)) return 0;
                } else if (!org_inline_delimited(parser, "*", "*",
                    text + i + 1u, end - i - 1u)) return 0;
                i = end + 1u;
                continue;
            }
        }
        if (!org_append_plain(parser, text + i, 1u)) return 0;
        ++i;
    }
    return 1;
}

static int org_collect_block(OrgParser *parser, const uint8_t *data, size_t size,
                             size_t *position, const char *end_directive,
                             int mode)
{
    QwTextBuffer content = {0};
    int found_end = 0;
    int rc = 1;
    content.limit = ORG_MAX_MARKDOWN_BYTES + 1u;
    while (*position < size) {
        OrgLine line;
        if (!org_read_line(data, size, position, &line)) break;
        if (org_directive(&line, end_directive, NULL)) {
            found_end = 1;
            break;
        }
        if (mode != 2) {
            if (line.size && !qw_text_buffer_append(&content,
                    (const char *)line.data, line.size)) {
                parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
                rc = 0;
                goto cleanup;
            }
            if (!qw_text_buffer_append(&content, "\n", 1u)) {
                parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
                rc = 0;
                goto cleanup;
            }
        }
    }
    if (mode == 0) {
        size_t fence = org_max_backticks((const uint8_t *)content.data,
                                         content.size) + 1u;
        if (fence < 3u) fence = 3u;
        if (!org_literal(parser, "\n") || !org_repeated(parser, '`', fence) ||
            !org_literal(parser, "\n") ||
            (content.size && !org_append(parser, content.data, content.size)) ||
            !org_repeated(parser, '`', fence) ||
            !org_literal(parser, "\n\n")) rc = 0;
    } else if (mode == 1) {
        size_t content_position = 0u;
        if (!org_literal(parser, "\n")) rc = 0;
        while (rc && content_position < content.size) {
            OrgLine line;
            if (!org_read_line((const uint8_t *)content.data, content.size,
                               &content_position, &line)) break;
            if (!org_literal(parser, "> ") ||
                !org_inline(parser, line.data, line.size) ||
                !org_literal(parser, "\n")) rc = 0;
        }
        if (rc && !org_literal(parser, "\n")) rc = 0;
    }
    if (!rc && !parser->error) parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
    if (!found_end && mode == 2) parser->omitted_export = 1;

cleanup:
    free(content.data);
    return rc;
}

static int org_list_marker(const uint8_t *text, size_t size,
                           size_t *out_marker_size, int *out_ordered)
{
    size_t i;
    if (size >= 2u && (text[0] == '-' || text[0] == '+') && text[1] == ' ') {
        *out_marker_size = 2u;
        *out_ordered = 0;
        return 1;
    }
    i = 0u;
    while (i < size && text[i] >= '0' && text[i] <= '9') ++i;
    if (i && i + 1u < size && (text[i] == '.' || text[i] == ')') &&
        text[i + 1u] == ' ') {
        *out_marker_size = i + 2u;
        *out_ordered = 1;
        return 1;
    }
    return 0;
}

static int org_translate(const uint8_t *data, size_t size, OrgParser *parser)
{
    size_t position = 0u;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    while (position < size) {
        OrgLine line;
        size_t indent = 0u;
        size_t content_start;
        size_t content_size;
        size_t marker_size;
        int ordered;
        if (!org_read_line(data, size, &position, &line)) break;
        while (indent < line.size &&
               (line.data[indent] == ' ' || line.data[indent] == '\t')) ++indent;
        content_start = indent;
        content_size = line.size - content_start;
        if (org_blank(&line)) {
            if (!org_literal(parser, "\n")) return 0;
            continue;
        }
        if (content_size >= 2u && line.data[content_start] == '*') {
            size_t level = 0u;
            while (level < content_size && line.data[content_start + level] == '*')
                ++level;
            if (level && level < content_size && line.data[content_start + level] == ' ') {
                if (level > 6u) level = 6u;
                if (!org_literal(parser, "\n") || !org_repeated(parser, '#', level) ||
                    !org_literal(parser, " ") ||
                    !org_inline(parser, line.data + content_start + level + 1u,
                                content_size - level - 1u) ||
                    !org_literal(parser, "\n\n")) return 0;
                continue;
            }
        }
        if (org_directive(&line, "#+begin_src", NULL) ||
            org_directive(&line, "#+begin_example", NULL)) {
            const char *end_name = org_directive(&line, "#+begin_src", NULL)
                ? "#+end_src" : "#+end_example";
            if (!org_collect_block(parser, data, size, &position, end_name, 0))
                return 0;
            continue;
        }
        if (org_directive(&line, "#+begin_quote", NULL)) {
            if (!org_collect_block(parser, data, size, &position,
                                   "#+end_quote", 1)) return 0;
            continue;
        }
        if (org_directive(&line, "#+begin_export", NULL)) {
            if (!org_collect_block(parser, data, size, &position,
                                   "#+end_export", 2)) return 0;
            parser->omitted_export = 1;
            continue;
        }
        if (org_equal(line.data + indent, line.size - indent, ":PROPERTIES:")) {
            while (position < size) {
                OrgLine property;
                if (!org_read_line(data, size, &position, &property)) break;
                if (org_equal(property.data, property.size, ":END:")) break;
            }
            continue;
        }
        if ((content_size >= 2u && line.data[content_start] == '#' &&
             line.data[content_start + 1u] == '+') ||
            (content_size >= 2u && line.data[content_start] == '#' &&
             line.data[content_start + 1u] == ' ')) continue;
        if (content_size >= 5u && line.data[content_start] == '-') {
            size_t i = 0u;
            while (i < content_size && line.data[content_start + i] == '-') ++i;
            if (i >= 5u) {
                if (!org_literal(parser, "\n---\n\n")) return 0;
                continue;
            }
        }
        if (org_list_marker(line.data + content_start, content_size,
                            &marker_size, &ordered)) {
            if (indent && !org_repeated(parser, ' ', indent)) return 0;
            if (ordered) {
                size_t digit_size = marker_size - 2u;
                if (!org_append(parser, (const char *)line.data + content_start,
                                digit_size) ||
                    !org_literal(parser, ". ")) return 0;
            } else if (!org_literal(parser, "- ")) {
                return 0;
            }
            if (!org_inline(parser, line.data + content_start + marker_size,
                            content_size - marker_size) ||
                !org_literal(parser, "\n")) return 0;
            continue;
        }
        if (!org_inline(parser, line.data + indent, line.size - indent) ||
            !org_literal(parser, "\n")) return 0;
    }
    if (!parser->markdown.size && !org_literal(parser, "\n")) return 0;
    return 1;
}

int qw_org_abi_version(void)
{
    return QW_ORG_ABI_VERSION;
}

QwOrgStatus qw_org_parse_markdown(const void *data, size_t size,
                                  QwOrgDocument *out_document)
{
    OrgParser parser;
    if (!data || !size || !out_document) return QW_ORG_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (size > ORG_MAX_INPUT_BYTES) return QW_ORG_ERR_RESOURCE_LIMIT;
    memset(&parser, 0, sizeof(parser));
    parser.markdown.limit = ORG_MAX_MARKDOWN_BYTES + 1u;
    if (!org_translate((const uint8_t *)data, size, &parser)) {
        QwOrgStatus status = parser.error == QW_STATUS_ERR_RESOURCE_LIMIT
            ? QW_ORG_ERR_RESOURCE_LIMIT : QW_ORG_ERR_OUT_OF_MEMORY;
        free(parser.markdown.data);
        return status;
    }
    out_document->markdown = parser.markdown.data;
    out_document->markdown_size = parser.markdown.size;
    out_document->omitted_export_blocks = parser.omitted_export;
    out_document->dropped_unsafe_links = parser.dropped_link;
    return QW_ORG_OK;
}

void qw_org_document_free(QwOrgDocument *document)
{
    if (!document) return;
    free(document->markdown);
    memset(document, 0, sizeof(*document));
}

const char *qw_org_status_string(QwOrgStatus status)
{
    switch (status) {
    case QW_ORG_OK: return "success";
    case QW_ORG_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_ORG_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_ORG_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    default: return "unknown Org parser status";
    }
}

#ifndef QUIREWISE_STANDALONE_ORG_PARSER

static int org_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t position = 0u;
    size_t scan_size;
    if (!input || !size || size > ORG_MAX_INPUT_BYTES) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbf)
        position = 3u;
    scan_size = size < ORG_PROBE_BYTES ? size : ORG_PROBE_BYTES;
    while (position < scan_size) {
        OrgLine line;
        size_t indent = 0u;
        if (!org_read_line(data, scan_size, &position, &line)) break;
        while (indent < line.size &&
               (line.data[indent] == ' ' || line.data[indent] == '\t')) ++indent;
        if (org_directive(&line, "#+title:", NULL) ||
            org_directive(&line, "#+begin_src", NULL) ||
            org_directive(&line, "#+begin_quote", NULL) ||
            org_directive(&line, "#+begin_export", NULL) ||
            org_equal(line.data + indent, line.size - indent, ":PROPERTIES:"))
            return 1;
        if (line.size >= 5u &&
            org_contains_pair(line.data, line.size, '[', '[') &&
            org_contains_pair(line.data, line.size, ']', '[')) return 1;
        if (memchr(line.data, '=', line.size) || memchr(line.data, '~', line.size)) {
            const uint8_t *equal = (const uint8_t *)memchr(line.data, '=', line.size);
            const uint8_t *tilde = (const uint8_t *)memchr(line.data, '~', line.size);
            if ((equal && memchr(equal + 1u, '=',
                    line.size - (size_t)(equal + 1u - line.data))) ||
                (tilde && memchr(tilde + 1u, '~',
                    line.size - (size_t)(tilde + 1u - line.data)))) return 1;
        }
    }
    return 0;
}

static int org_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    return dot && org_equal((const uint8_t *)dot, strlen(dot), ".org");
}

static int org_open_layout(const void *input, size_t input_size,
                           QwInputFormat input_format,
                           const QwConvertOptions *options,
                           QwLayoutDocument *out_document,
                           QwConvertReport *report)
{
    QwOrgDocument org = {0};
    int omitted_export_blocks;
    int dropped_unsafe_links;
    int rc;
    (void)options;
    if (!input || !input_size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_ORG) return QW_STATUS_ERR_INVALID_ARGUMENT;
    {
        QwOrgStatus status = qw_org_parse_markdown(input, input_size, &org);
        if (status == QW_ORG_ERR_INVALID_ARGUMENT)
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        if (status == QW_ORG_ERR_RESOURCE_LIMIT) return QW_STATUS_ERR_RESOURCE_LIMIT;
        if (status != QW_ORG_OK) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    omitted_export_blocks = org.omitted_export_blocks;
    dropped_unsafe_links = org.dropped_unsafe_links;
    rc = qw_markdown_format_open_layout(org.markdown,
                                        org.markdown_size,
                                        QW_FORMAT_MARKDOWN, NULL,
                                        out_document, report);
    qw_org_document_free(&org);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        (void)snprintf(report->note, sizeof(report->note),
            "Org subset rendered through CommonMark; tables, drawers, Babel execution and export backends are not interpreted%s%s",
            omitted_export_blocks ? "; export blocks were omitted" : "",
            dropped_unsafe_links ? "; unsafe links were reduced to label text" : "");
    }
    return rc;
}

const QwFormatDescriptor qw_format_org = {
    .id = QW_FORMAT_ORG,
    .name = "Org mode",
    .probe = org_probe,
    .open_layout = org_open_layout,
    .max_input_bytes = ORG_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};

const QwPathFormatDescriptor qw_path_format_org = {
    .id = QW_FORMAT_ORG,
    .probe_path = org_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};

#endif

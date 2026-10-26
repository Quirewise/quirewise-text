#ifndef QUIREWISE_STANDALONE_TEXTILE_PARSER
#include "../../core/format_registry.h"
#include "markdown_format.h"
#endif
#include "../../core/text_buffer.h"

#include <quirewise/textile.h>
#include "quirewise/status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEXTILE_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    TEXTILE_MAX_MARKDOWN_BYTES = 128u * 1024u * 1024u,
    TEXTILE_PROBE_BYTES = 65536u
};

typedef struct TextileLine {
    const uint8_t *data;
    size_t size;
} TextileLine;

typedef struct TextileParser {
    QwTextBuffer markdown;
    int error;
    int omitted_image;
    int dropped_link;
} TextileParser;

static int textile_append(TextileParser *parser, const char *text, size_t size)
{
    if (parser->markdown.size > TEXTILE_MAX_MARKDOWN_BYTES ||
        size > TEXTILE_MAX_MARKDOWN_BYTES - parser->markdown.size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&parser->markdown, text, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int textile_literal(TextileParser *parser, const char *text)
{
    return textile_append(parser, text, strlen(text));
}

static int textile_repeated(TextileParser *parser, char value, size_t count)
{
    char chunk[32];
    memset(chunk, value, sizeof(chunk));
    while (count) {
        size_t amount = count < sizeof(chunk) ? count : sizeof(chunk);
        if (!textile_append(parser, chunk, amount)) return 0;
        count -= amount;
    }
    return 1;
}

static int textile_read_line(const uint8_t *data, size_t size,
                             size_t *position, TextileLine *out_line)
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

static size_t textile_indent(const TextileLine *line, size_t *out_bytes)
{
    size_t bytes = 0u;
    size_t columns = 0u;
    while (bytes < line->size &&
           (line->data[bytes] == ' ' || line->data[bytes] == '\t')) {
        columns = line->data[bytes] == '\t' ? (columns + 8u) & ~(size_t)7u
                                             : columns + 1u;
        ++bytes;
    }
    if (out_bytes) *out_bytes = bytes;
    return columns;
}

static int textile_blank(const TextileLine *line)
{
    size_t i;
    for (i = 0u; i < line->size; ++i) {
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    }
    return 1;
}

static int textile_prefix(const uint8_t *text, size_t size,
                          const char *prefix, size_t *out_size)
{
    size_t prefix_size = strlen(prefix);
    if (size < prefix_size || memcmp(text, prefix, prefix_size) != 0) return 0;
    if (out_size) *out_size = prefix_size;
    return 1;
}

static int textile_url_safe(const uint8_t *url, size_t size)
{
    size_t i;
    int valid_scheme =
        (size >= 7u && memcmp(url, "http://", 7u) == 0) ||
        (size >= 8u && memcmp(url, "https://", 8u) == 0) ||
        (size >= 7u && memcmp(url, "mailto:", 7u) == 0);
    if (!valid_scheme) return 0;
    for (i = 0u; i < size; ++i) {
        if (url[i] <= 0x20u || url[i] == 0x7fu || url[i] == '<' ||
            url[i] == '>' || url[i] == '"' || url[i] == '\\') return 0;
    }
    return 1;
}

static int textile_append_plain(TextileParser *parser,
                                const uint8_t *text, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        const char *replacement = NULL;
        switch (text[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '\\': replacement = "\\\\"; break;
        case '`': replacement = "\\`"; break;
        case '*': replacement = "\\*"; break;
        case '_': replacement = "\\_"; break;
        case '[': replacement = "\\["; break;
        case ']': replacement = "\\]"; break;
        case '#': replacement = "\\#"; break;
        case '~': replacement = "\\~"; break;
        default: break;
        }
        if (replacement) {
            if (!textile_literal(parser, replacement)) return 0;
        } else if (!textile_append(parser, (const char *)text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

static size_t textile_max_backticks(const uint8_t *text, size_t size)
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

static int textile_append_code(TextileParser *parser,
                               const uint8_t *text, size_t size)
{
    size_t fence = textile_max_backticks(text, size) + 1u;
    if (fence < 1u) fence = 1u;
    if (!textile_repeated(parser, '`', fence)) return 0;
    if (size && (text[0] == ' ' || text[0] == '`') &&
        (text[size - 1u] == ' ' || text[size - 1u] == '`')) {
        if (!textile_literal(parser, " ") ||
            !textile_append(parser, (const char *)text, size) ||
            !textile_literal(parser, " ")) return 0;
    } else if (!textile_append(parser, (const char *)text, size)) {
        return 0;
    }
    return textile_repeated(parser, '`', fence);
}

static int textile_append_delimited(TextileParser *parser,
                                    const char *markdown_open,
                                    const char *markdown_close,
                                    const uint8_t *text, size_t size)
{
    return textile_literal(parser, markdown_open) &&
           textile_append_plain(parser, text, size) &&
           textile_literal(parser, markdown_close);
}

static int textile_inline(TextileParser *parser,
                          const uint8_t *text, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        if (text[i] == '@') {
            size_t end = i + 1u;
            while (end < size && text[end] != '@') ++end;
            if (end < size && end > i + 1u) {
                if (!textile_append_code(parser, text + i + 1u,
                                         end - i - 1u)) return 0;
                i = end + 1u;
                continue;
            }
        }
        if (text[i] == '"') {
            size_t label_end = i + 1u;
            while (label_end < size && text[label_end] != '"') ++label_end;
            if (label_end < size && label_end + 1u < size &&
                text[label_end + 1u] == ':') {
                size_t url_start = label_end + 2u;
                size_t url_end = url_start;
                while (url_end < size && text[url_end] != ' ' &&
                       text[url_end] != '\t') ++url_end;
                if (url_end > url_start &&
                    textile_url_safe(text + url_start, url_end - url_start)) {
                    if (!textile_literal(parser, "[") ||
                        !textile_append_plain(parser, text + i + 1u,
                                              label_end - i - 1u) ||
                        !textile_literal(parser, "](<") ||
                        !textile_append(parser, (const char *)text + url_start,
                                        url_end - url_start) ||
                        !textile_literal(parser, ">)")) return 0;
                } else {
                    parser->dropped_link = 1;
                    if (!textile_append_plain(parser, text + i + 1u,
                                              label_end - i - 1u)) return 0;
                }
                i = url_end;
                continue;
            }
        }
        if (text[i] == '!') {
            size_t end = i + 1u;
            while (end < size && text[end] != '!') ++end;
            if (end < size && end > i + 1u) {
                size_t content_start = i + 1u;
                size_t content_size = end - content_start;
                size_t open = content_size;
                size_t j;
                int path_like = 0;
                int has_space = 0;
                for (j = 0u; j < content_size; ++j) {
                    if (text[content_start + j] <= 0x20u) has_space = 1;
                    if (text[content_start + j] == '(') open = j;
                    if (text[content_start + j] == '/' ||
                        text[content_start + j] == '.') path_like = 1;
                }
                path_like = path_like && !has_space;
                if (!path_like) {
                    /* Exclamation punctuation is common prose, not an image. */
                } else if (open < content_size && content_size > open + 2u &&
                    text[content_start + content_size - 1u] == ')') {
                    if (!textile_literal(parser, "[Image omitted: ") ||
                        !textile_append_plain(parser,
                            text + content_start + open + 1u,
                            content_size - open - 2u) ||
                        !textile_literal(parser, "]")) return 0;
                } else if (!textile_literal(parser, "[Image omitted]")) {
                    return 0;
                }
                if (path_like) {
                    parser->omitted_image = 1;
                    i = end + 1u;
                    continue;
                }
            }
        }
        if (text[i] == '*') {
            size_t end = i + 1u;
            while (end < size && text[end] != '*') ++end;
            if (end < size && end > i + 1u && text[i + 1u] != ' ' &&
                text[end - 1u] != ' ') {
                if (!textile_append_delimited(parser, "**", "**",
                    text + i + 1u, end - i - 1u)) return 0;
                i = end + 1u;
                continue;
            }
        }
        if (text[i] == '_') {
            size_t end = i + 1u;
            while (end < size && text[end] != '_') ++end;
            if (end < size && end > i + 1u && text[i + 1u] != ' ' &&
                text[end - 1u] != ' ') {
                if (!textile_append_delimited(parser, "*", "*",
                    text + i + 1u, end - i - 1u)) return 0;
                i = end + 1u;
                continue;
            }
        }
        if (text[i] == '=' && i + 1u < size && text[i + 1u] == '=') {
            size_t end = i + 2u;
            while (end + 1u < size &&
                   !(text[end] == '=' && text[end + 1u] == '=')) ++end;
            if (end + 1u < size) {
                if (!textile_append_plain(parser, text + i + 2u,
                                          end - i - 2u)) return 0;
                i = end + 2u;
                continue;
            }
        }
        if (!textile_append_plain(parser, text + i, 1u)) return 0;
        ++i;
    }
    return 1;
}

static int textile_heading(const uint8_t *text, size_t size,
                           size_t *out_level, size_t *out_content)
{
    size_t level;
    if (size < 4u || text[0] != 'h' || text[1] < '1' || text[1] > '6' ||
        text[2] != '.' || text[3] != ' ') return 0;
    level = (size_t)(text[1] - '0');
    if (out_level) *out_level = level;
    if (out_content) *out_content = 4u;
    return 1;
}

static int textile_list(const uint8_t *text, size_t size,
                        size_t *out_depth, int *out_ordered,
                        size_t *out_content)
{
    uint8_t marker;
    size_t depth = 0u;
    if (!size || (text[0] != '*' && text[0] != '#')) return 0;
    marker = text[0];
    while (depth < size && text[depth] == marker) ++depth;
    if (depth >= size || text[depth] != ' ' || depth > 6u) return 0;
    if (out_depth) *out_depth = depth;
    if (out_ordered) *out_ordered = marker == '#';
    if (out_content) *out_content = depth + 1u;
    return 1;
}

static int textile_emit_code_block(TextileParser *parser,
                                   const uint8_t *data, size_t size,
                                   size_t *position, const uint8_t *first,
                                   size_t first_size)
{
    QwTextBuffer code = {0};
    size_t fence;
    int rc = 1;
    code.limit = TEXTILE_MAX_MARKDOWN_BYTES + 1u;
    if (first_size && !qw_text_buffer_append(&code, (const char *)first,
                                              first_size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        rc = 0;
        goto cleanup;
    }
    if (!qw_text_buffer_append(&code, "\n", 1u)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        rc = 0;
        goto cleanup;
    }
    while (*position < size) {
        size_t before = *position;
        TextileLine line;
        if (!textile_read_line(data, size, position, &line)) break;
        if (textile_blank(&line)) {
            *position = before;
            break;
        }
        if (line.size && !qw_text_buffer_append(&code, (const char *)line.data,
                                                 line.size)) {
            parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
            rc = 0;
            goto cleanup;
        }
        if (!qw_text_buffer_append(&code, "\n", 1u)) {
            parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
            rc = 0;
            goto cleanup;
        }
    }
    fence = textile_max_backticks((const uint8_t *)code.data, code.size) + 1u;
    if (fence < 3u) fence = 3u;
    if (!textile_literal(parser, "\n") ||
        !textile_repeated(parser, '`', fence) ||
        !textile_literal(parser, "\n") ||
        (code.size && !textile_append(parser, code.data, code.size)) ||
        !textile_repeated(parser, '`', fence) ||
        !textile_literal(parser, "\n\n")) rc = 0;
    if (!rc && !parser->error) parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;

cleanup:
    free(code.data);
    return rc;
}

static int textile_translate(const uint8_t *data, size_t size,
                             TextileParser *parser)
{
    size_t position = 0u;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    while (position < size) {
        TextileLine line;
        size_t indent_bytes;
        size_t line_indent;
        size_t content_size;
        size_t level;
        size_t content;
        size_t depth;
        int ordered;
        int literal_line = 0;
        if (!textile_read_line(data, size, &position, &line)) break;
        line_indent = textile_indent(&line, &indent_bytes);
        content_size = line.size - indent_bytes;
        if (textile_blank(&line)) {
            if (!textile_literal(parser, "\n")) return 0;
            continue;
        }
        if (textile_heading(line.data + indent_bytes, content_size,
                            &level, &content)) {
            if (!textile_literal(parser, "\n") ||
                !textile_repeated(parser, '#', level) ||
                !textile_literal(parser, " ") ||
                !textile_inline(parser, line.data + indent_bytes + content,
                                content_size - content) ||
                !textile_literal(parser, "\n\n")) return 0;
            continue;
        }
        if (textile_list(line.data + indent_bytes, content_size,
                         &depth, &ordered, &content)) {
            size_t nesting = depth - 1u;
            if (!textile_repeated(parser, ' ', line_indent + nesting * 2u) ||
                !textile_literal(parser, ordered ? "1. " : "- ") ||
                !textile_inline(parser, line.data + indent_bytes + content,
                                content_size - content) ||
                !textile_literal(parser, "\n")) return 0;
            continue;
        }
        {
            static const struct { const char *prefix; const char *output; } blocks[] = {
                {"p. ", ""}, {"bq. ", "> "}
            };
            size_t i;
            for (i = 0u; i < sizeof(blocks) / sizeof(blocks[0]); ++i) {
                size_t prefix_size;
                if (textile_prefix(line.data + indent_bytes, content_size,
                                   blocks[i].prefix, &prefix_size)) {
                    if (!textile_literal(parser, blocks[i].output) ||
                        !textile_inline(parser,
                            line.data + indent_bytes + prefix_size,
                            content_size - prefix_size) ||
                        !textile_literal(parser, "\n")) return 0;
                    break;
                }
            }
            if (i < sizeof(blocks) / sizeof(blocks[0])) continue;
        }
        {
            size_t prefix_size;
            if (textile_prefix(line.data + indent_bytes, content_size,
                               "bc. ", &prefix_size) ||
                textile_prefix(line.data + indent_bytes, content_size,
                               "pre. ", &prefix_size)) {
                if (!textile_emit_code_block(parser, data, size, &position,
                    line.data + indent_bytes + prefix_size,
                    content_size - prefix_size)) return 0;
                continue;
            }
        }
        {
            size_t prefix_size;
            if (textile_prefix(line.data + indent_bytes, content_size,
                               "notextile. ", &prefix_size)) {
                literal_line = 1;
                if (!textile_append_plain(parser,
                    line.data + indent_bytes + prefix_size,
                    content_size - prefix_size) ||
                    !textile_literal(parser, "\n")) return 0;
            }
        }
        if (literal_line) continue;
        if (!textile_inline(parser, line.data + indent_bytes, content_size) ||
            !textile_literal(parser, "\n")) return 0;
    }
    if (!parser->markdown.size && !textile_literal(parser, "\n")) return 0;
    return 1;
}

int qw_textile_abi_version(void)
{
    return QW_TEXTILE_ABI_VERSION;
}

QwTextileStatus qw_textile_parse_markdown(const void *data, size_t size,
                                         QwTextileDocument *out_document)
{
    TextileParser parser;
    if (!data || !size || !out_document) return QW_TEXTILE_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (size > TEXTILE_MAX_INPUT_BYTES) return QW_TEXTILE_ERR_RESOURCE_LIMIT;
    memset(&parser, 0, sizeof(parser));
    parser.markdown.limit = TEXTILE_MAX_MARKDOWN_BYTES + 1u;
    if (!textile_translate((const uint8_t *)data, size, &parser)) {
        QwTextileStatus status = parser.error == QW_STATUS_ERR_RESOURCE_LIMIT
            ? QW_TEXTILE_ERR_RESOURCE_LIMIT : QW_TEXTILE_ERR_OUT_OF_MEMORY;
        free(parser.markdown.data);
        return status;
    }
    out_document->markdown = parser.markdown.data;
    out_document->markdown_size = parser.markdown.size;
    out_document->omitted_images = parser.omitted_image;
    out_document->dropped_unsafe_links = parser.dropped_link;
    return QW_TEXTILE_OK;
}

void qw_textile_document_free(QwTextileDocument *document)
{
    if (!document) return;
    free(document->markdown);
    memset(document, 0, sizeof(*document));
}

const char *qw_textile_status_string(QwTextileStatus status)
{
    switch (status) {
    case QW_TEXTILE_OK: return "success";
    case QW_TEXTILE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_TEXTILE_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_TEXTILE_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    default: return "unknown Textile parser status";
    }
}

#ifndef QUIREWISE_STANDALONE_TEXTILE_PARSER

static int textile_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t scan_size;
    size_t position = 0u;
    if (!input || !size || size > TEXTILE_MAX_INPUT_BYTES) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    scan_size = size < TEXTILE_PROBE_BYTES ? size : TEXTILE_PROBE_BYTES;
    while (position < scan_size) {
        TextileLine line;
        size_t indent_bytes;
        size_t content_size;
        size_t level;
        size_t content;
        if (!textile_read_line(data, scan_size, &position, &line)) break;
        (void)textile_indent(&line, &indent_bytes);
        content_size = line.size - indent_bytes;
        if (textile_heading(line.data + indent_bytes, content_size,
                            &level, &content)) return 1;
        if (textile_prefix(line.data + indent_bytes, content_size, "p. ", NULL) ||
            textile_prefix(line.data + indent_bytes, content_size, "bq. ", NULL) ||
            textile_prefix(line.data + indent_bytes, content_size, "bc. ", NULL) ||
            textile_prefix(line.data + indent_bytes, content_size, "pre. ", NULL) ||
            textile_prefix(line.data + indent_bytes, content_size,
                           "notextile. ", NULL)) return 1;
        if (memchr(line.data, '@', line.size)) {
            const uint8_t *cursor = line.data;
            size_t remaining = line.size;
            while (remaining) {
                const uint8_t *open = (const uint8_t *)memchr(cursor, '@', remaining);
                const uint8_t *close;
                int token_start;
                int token_end;
                if (!open) break;
                remaining = line.size - (size_t)(open + 1u - line.data);
                close = (const uint8_t *)memchr(open + 1u, '@', remaining);
                if (!close) break;
                token_start = open == line.data || open[-1] <= 0x20u ||
                    open[-1] == '(' || open[-1] == '[' || open[-1] == '{';
                token_end = close + 1u == line.data + line.size ||
                    close[1] <= 0x20u || close[1] == ')' || close[1] == ']' ||
                    close[1] == '}' || close[1] == ',' || close[1] == '.' ||
                    close[1] == ';' || close[1] == ':' || close[1] == '!';
                if (token_start && token_end && close > open + 1u) {
                    const uint8_t *inner;
                    int compact = 1;
                    for (inner = open + 1u; inner < close; ++inner) {
                        if (*inner <= 0x20u) {
                            compact = 0;
                            break;
                        }
                    }
                    if (compact) return 1;
                }
                cursor = close + 1u;
                remaining = line.size - (size_t)(cursor - line.data);
            }
        }
        if (memchr(line.data, '"', line.size) &&
            memchr(line.data, ':', line.size)) {
            size_t i;
            for (i = 0u; i + 1u < line.size; ++i) {
                if (line.data[i] == '"' && line.data[i + 1u] != '\0') {
                    size_t end = i + 1u;
                    while (end < line.size && line.data[end] != '"') ++end;
                    if (end + 2u < line.size && line.data[end + 1u] == ':' &&
                        (textile_prefix(line.data + end + 2u,
                            line.size - end - 2u, "http://", NULL) ||
                         textile_prefix(line.data + end + 2u,
                            line.size - end - 2u, "https://", NULL) ||
                         textile_prefix(line.data + end + 2u,
                            line.size - end - 2u, "mailto:", NULL))) return 1;
                }
            }
        }
    }
    return 0;
}

static int textile_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    if (!dot || strlen(dot) != 8u) return 0;
    return (dot[1] == 't' || dot[1] == 'T') &&
           (dot[2] == 'e' || dot[2] == 'E') &&
           (dot[3] == 'x' || dot[3] == 'X') &&
           (dot[4] == 't' || dot[4] == 'T') &&
           (dot[5] == 'i' || dot[5] == 'I') &&
           (dot[6] == 'l' || dot[6] == 'L') &&
           (dot[7] == 'e' || dot[7] == 'E');
}

static int textile_open_layout(const void *input, size_t input_size,
                               QwInputFormat input_format,
                               const QwConvertOptions *options,
                               QwLayoutDocument *out_document,
                               QwConvertReport *report)
{
    QwTextileDocument textile = {0};
    int omitted_images;
    int dropped_unsafe_links;
    int rc;
    (void)options;
    if (!input || !input_size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_TEXTILE) return QW_STATUS_ERR_INVALID_ARGUMENT;
    {
        QwTextileStatus status = qw_textile_parse_markdown(
            input, input_size, &textile);
        if (status == QW_TEXTILE_ERR_INVALID_ARGUMENT)
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        if (status == QW_TEXTILE_ERR_RESOURCE_LIMIT)
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        if (status != QW_TEXTILE_OK) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    omitted_images = textile.omitted_images;
    dropped_unsafe_links = textile.dropped_unsafe_links;
    rc = qw_markdown_format_open_layout(textile.markdown,
                                        textile.markdown_size,
                                        QW_FORMAT_MARKDOWN, NULL,
                                        out_document, report);
    qw_textile_document_free(&textile);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        (void)snprintf(report->note, sizeof(report->note),
            "Textile subset rendered through CommonMark; tables, attributes, references and raw HTML are not interpreted%s%s",
            omitted_images ? "; images were replaced by text" : "",
            dropped_unsafe_links ? "; unsafe links were reduced to label text" : "");
    }
    return rc;
}

const QwFormatDescriptor qw_format_textile = {
    .id = QW_FORMAT_TEXTILE,
    .name = "Textile",
    .probe = textile_probe,
    .open_layout = textile_open_layout,
    .max_input_bytes = TEXTILE_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};

const QwPathFormatDescriptor qw_path_format_textile = {
    .id = QW_FORMAT_TEXTILE,
    .probe_path = textile_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};

#endif

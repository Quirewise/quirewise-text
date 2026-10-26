#ifndef QUIREWISE_STANDALONE_MEDIAWIKI_PARSER
#include "../../core/format_registry.h"
#include "markdown_format.h"
#endif
#include "../../core/text_buffer.h"

#include <quirewise/mediawiki.h>
#include "quirewise/status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MEDIAWIKI_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    MEDIAWIKI_MAX_MARKDOWN_BYTES = 128u * 1024u * 1024u,
    MEDIAWIKI_PROBE_BYTES = 65536u
};

typedef struct MediaWikiLine {
    const uint8_t *data;
    size_t size;
} MediaWikiLine;

typedef struct MediaWikiParser {
    QwTextBuffer markdown;
    int error;
    int in_comment;
    int dropped_link;
    int omitted_media;
} MediaWikiParser;

static int mediawiki_append(MediaWikiParser *parser,
                            const char *text, size_t size)
{
    if (parser->markdown.size > MEDIAWIKI_MAX_MARKDOWN_BYTES ||
        size > MEDIAWIKI_MAX_MARKDOWN_BYTES - parser->markdown.size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&parser->markdown, text, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int mediawiki_literal(MediaWikiParser *parser, const char *text)
{
    return mediawiki_append(parser, text, strlen(text));
}

static int mediawiki_repeated(MediaWikiParser *parser, char value,
                              size_t count)
{
    char chunk[32];
    memset(chunk, value, sizeof(chunk));
    while (count) {
        size_t amount = count < sizeof(chunk) ? count : sizeof(chunk);
        if (!mediawiki_append(parser, chunk, amount)) return 0;
        count -= amount;
    }
    return 1;
}

static int mediawiki_read_line(const uint8_t *data, size_t size,
                               size_t *position, MediaWikiLine *out_line)
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

static uint8_t mediawiki_lower(uint8_t value)
{
    return value >= 'A' && value <= 'Z'
        ? (uint8_t)(value + ('a' - 'A')) : value;
}

static int mediawiki_equal(const uint8_t *text, size_t size,
                           const char *literal)
{
    size_t i;
    size_t length = strlen(literal);
    if (size != length) return 0;
    for (i = 0u; i < size; ++i)
        if (mediawiki_lower(text[i]) != (uint8_t)literal[i]) return 0;
    return 1;
}

static int mediawiki_prefix(const uint8_t *text, size_t size,
                            const char *literal)
{
    size_t i;
    size_t length = strlen(literal);
    if (size < length) return 0;
    for (i = 0u; i < length; ++i)
        if (mediawiki_lower(text[i]) != (uint8_t)literal[i]) return 0;
    return 1;
}

static size_t mediawiki_find(const uint8_t *text, size_t size,
                             const char *needle, size_t start)
{
    size_t needle_size = strlen(needle);
    size_t i;
    if (!needle_size || start > size || needle_size > size - start)
        return SIZE_MAX;
    for (i = start; i <= size - needle_size; ++i)
        if (memcmp(text + i, needle, needle_size) == 0) return i;
    return SIZE_MAX;
}

static int mediawiki_blank(const MediaWikiLine *line)
{
    size_t i;
    for (i = 0u; i < line->size; ++i)
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    return 1;
}

static int mediawiki_url_safe(const uint8_t *url, size_t size)
{
    size_t i;
    int scheme = (size >= 7u && mediawiki_equal(url, 7u, "http://")) ||
                 (size >= 8u && mediawiki_equal(url, 8u, "https://")) ||
                 (size >= 7u && mediawiki_equal(url, 7u, "mailto:"));
    if (!scheme) return 0;
    for (i = 0u; i < size; ++i)
        if (url[i] <= 0x20u || url[i] == 0x7fu || url[i] == '<' ||
            url[i] == '>' || url[i] == '"' || url[i] == '\\' ||
            url[i] == '[' || url[i] == ']') return 0;
    return 1;
}

static int mediawiki_append_plain(MediaWikiParser *parser,
                                  const uint8_t *text, size_t size)
{
    size_t i;
    size_t span = 0u;
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
            if ((i > span && !mediawiki_append(parser,
                    (const char *)text + span, i - span)) ||
                !mediawiki_literal(parser, replacement)) return 0;
            span = i + 1u;
        }
    }
    return span == size || mediawiki_append(parser,
        (const char *)text + span, size - span);
}

static int mediawiki_inline(MediaWikiParser *parser,
                            const uint8_t *text, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        if (parser->in_comment) {
            size_t end = mediawiki_find(text, size, "-->", i);
            if (end == SIZE_MAX) return 1;
            parser->in_comment = 0;
            i = end + 3u;
            continue;
        }
        if (i + 4u <= size && memcmp(text + i, "<!--", 4u) == 0) {
            size_t end = mediawiki_find(text, size, "-->", i + 4u);
            if (end == SIZE_MAX) {
                parser->in_comment = 1;
                return 1;
            }
            i = end + 3u;
            continue;
        }
        if (i + 1u < size && text[i] == '[' && text[i + 1u] == '[') {
            size_t end = mediawiki_find(text, size, "]]", i + 2u);
            if (end == SIZE_MAX) {
                if (!mediawiki_append_plain(parser, text + i, size - i)) return 0;
                return 1;
            }
            {
                size_t separator = i + 2u;
                size_t label_start;
                size_t label_size;
                while (separator < end && text[separator] != '|') ++separator;
                label_start = separator < end ? separator + 1u : i + 2u;
                label_size = end - label_start;
                if (separator == i + 2u) label_size = 0u;
                if (separator == end) label_size = end - (i + 2u);
                if (label_size &&
                    (mediawiki_prefix(text + i + 2u,
                                      separator - (i + 2u), "File:") ||
                     mediawiki_prefix(text + i + 2u,
                                      separator - (i + 2u), "Image:")))
                    parser->omitted_media = 1;
                if (!mediawiki_append_plain(parser, text + label_start,
                                             label_size)) return 0;
                i = end + 2u;
                continue;
            }
        }
        if (text[i] == '[' && i + 1u < size &&
            (mediawiki_prefix(text + i + 1u, size - i - 1u, "http://") ||
             mediawiki_prefix(text + i + 1u, size - i - 1u, "https://") ||
             mediawiki_prefix(text + i + 1u, size - i - 1u, "mailto:"))) {
            size_t end = i + 1u;
            size_t url_end;
            size_t label_start;
            while (end < size && text[end] != ']') ++end;
            if (end == size) {
                if (!mediawiki_append_plain(parser, text + i, size - i)) return 0;
                return 1;
            }
            url_end = i + 1u;
            while (url_end < end && text[url_end] > 0x20u) ++url_end;
            label_start = url_end;
            while (label_start < end &&
                   (text[label_start] == ' ' || text[label_start] == '\t'))
                ++label_start;
            if (mediawiki_url_safe(text + i + 1u, url_end - i - 1u)) {
                if (!mediawiki_literal(parser, "[") ||
                    !mediawiki_append_plain(parser,
                        text + (label_start < end ? label_start : i + 1u),
                        label_start < end ? end - label_start
                                          : url_end - i - 1u) ||
                    !mediawiki_literal(parser, "](<") ||
                    !mediawiki_append(parser, (const char *)text + i + 1u,
                                      url_end - i - 1u) ||
                    !mediawiki_literal(parser, ">)")) return 0;
            } else {
                parser->dropped_link = 1;
                if (label_start < end &&
                    !mediawiki_append_plain(parser, text + label_start,
                                            end - label_start)) return 0;
            }
            i = end + 1u;
            continue;
        }
        if (i + 2u < size && text[i] == '\'' && text[i + 1u] == '\'' &&
            text[i + 2u] == '\'') {
            size_t end = mediawiki_find(text, size, "'''", i + 3u);
            if (end != SIZE_MAX && end > i + 3u) {
                if (!mediawiki_literal(parser, "**") ||
                    !mediawiki_append_plain(parser, text + i + 3u,
                                            end - i - 3u) ||
                    !mediawiki_literal(parser, "**")) return 0;
                i = end + 3u;
                continue;
            }
            if (end == SIZE_MAX) {
                if (!mediawiki_append_plain(parser, text + i, size - i)) return 0;
                return 1;
            }
        }
        if (i + 1u < size && text[i] == '\'' && text[i + 1u] == '\'') {
            size_t end = mediawiki_find(text, size, "''", i + 2u);
            if (end != SIZE_MAX && end > i + 2u) {
                if (!mediawiki_literal(parser, "*") ||
                    !mediawiki_append_plain(parser, text + i + 2u,
                                            end - i - 2u) ||
                    !mediawiki_literal(parser, "*")) return 0;
                i = end + 2u;
                continue;
            }
            if (end == SIZE_MAX) {
                if (!mediawiki_append_plain(parser, text + i, size - i)) return 0;
                return 1;
            }
        }
        if (!mediawiki_append_plain(parser, text + i, 1u)) return 0;
        ++i;
    }
    return 1;
}

static int mediawiki_heading(const uint8_t *text, size_t size,
                             size_t *out_level, size_t *out_start,
                             size_t *out_size)
{
    size_t level = 0u;
    size_t end;
    size_t start;
    while (level < size && text[level] == '=') ++level;
    if (!level || level > 6u || size < level * 2u + 1u) return 0;
    end = size;
    while (end && text[end - 1u] == ' ') --end;
    if (end < level * 2u) return 0;
    for (start = 0u; start < level; ++start)
        if (text[end - level + start] != '=') return 0;
    start = level;
    while (start < end - level && text[start] == ' ') ++start;
    end -= level;
    while (end > start && text[end - 1u] == ' ') --end;
    if (start >= end) return 0;
    *out_level = level;
    *out_start = start;
    *out_size = end - start;
    return 1;
}

static int mediawiki_list(const uint8_t *text, size_t size,
                          size_t *out_depth, int *out_ordered,
                          size_t *out_content)
{
    size_t depth = 0u;
    uint8_t marker;
    if (!size || (text[0] != '*' && text[0] != '#')) return 0;
    marker = text[0];
    while (depth < size && text[depth] == marker) ++depth;
    if (depth >= size || text[depth] != ' ' || depth > 6u) return 0;
    *out_depth = depth;
    *out_ordered = marker == '#';
    *out_content = depth + 1u;
    return 1;
}

static size_t mediawiki_backtick_run(const uint8_t *text, size_t size)
{
    size_t i;
    size_t longest = 0u;
    size_t current = 0u;
    for (i = 0u; i < size; ++i) {
        if (text[i] == '`') {
            if (++current > longest) longest = current;
        } else {
            current = 0u;
        }
    }
    return longest;
}

static int mediawiki_code_block(MediaWikiParser *parser,
                                const uint8_t *data, size_t size,
                                size_t *position, const MediaWikiLine *first)
{
    QwTextBuffer code = {0};
    size_t fence;
    int rc = 1;
    code.limit = MEDIAWIKI_MAX_MARKDOWN_BYTES + 1u;
    if (first->size > 1u && !qw_text_buffer_append(&code,
            (const char *)first->data + 1u, first->size - 1u)) {
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
        MediaWikiLine line;
        if (!mediawiki_read_line(data, size, position, &line)) break;
        if (!line.size || line.data[0] != ' ') {
            *position = before;
            break;
        }
        if (line.size > 1u && !qw_text_buffer_append(&code,
                (const char *)line.data + 1u, line.size - 1u)) {
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
    fence = mediawiki_backtick_run((const uint8_t *)code.data, code.size) + 1u;
    if (fence < 3u) fence = 3u;
    if (!mediawiki_literal(parser, "\n")) { rc = 0; goto cleanup; }
    if (!mediawiki_repeated(parser, '`', fence)) { rc = 0; goto cleanup; }
    if (!mediawiki_literal(parser, "\n") ||
        (code.size && !mediawiki_append(parser, code.data, code.size))) {
        rc = 0;
        goto cleanup;
    }
    fence = mediawiki_backtick_run((const uint8_t *)code.data, code.size) + 1u;
    if (fence < 3u) fence = 3u;
    if (!mediawiki_repeated(parser, '`', fence)) { rc = 0; goto cleanup; }
    if (!mediawiki_literal(parser, "\n\n")) rc = 0;
    if (!rc && !parser->error) parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;

cleanup:
    free(code.data);
    return rc;
}

static int mediawiki_translate(const uint8_t *data, size_t size,
                               MediaWikiParser *parser)
{
    size_t position = 0u;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    while (position < size) {
        MediaWikiLine line;
        size_t indent = 0u;
        size_t level;
        size_t heading_start;
        size_t heading_size;
        size_t depth;
        size_t content;
        int ordered;
        if (!mediawiki_read_line(data, size, &position, &line)) break;
        if (mediawiki_blank(&line)) {
            if (!mediawiki_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.data[0] == ' ') {
            if (!mediawiki_code_block(parser, data, size, &position, &line))
                return 0;
            continue;
        }
        if (mediawiki_heading(line.data, line.size, &level,
                              &heading_start, &heading_size)) {
            if (!mediawiki_literal(parser, "\n") ||
                !mediawiki_literal(parser, "#")) return 0;
            while (--level) if (!mediawiki_literal(parser, "#")) return 0;
            if (!mediawiki_literal(parser, " ") ||
                !mediawiki_inline(parser, line.data + heading_start,
                                  heading_size) ||
                !mediawiki_literal(parser, "\n\n")) return 0;
            continue;
        }
        while (indent < line.size &&
               (line.data[indent] == '\t' || line.data[indent] == ' '))
            ++indent;
        if (mediawiki_list(line.data + indent, line.size - indent,
                           &depth, &ordered, &content)) {
            size_t spaces = indent + (depth - 1u) * 2u;
            while (spaces--) if (!mediawiki_literal(parser, " ")) return 0;
            if (!mediawiki_literal(parser, ordered ? "1. " : "- ") ||
                !mediawiki_inline(parser, line.data + indent + content,
                                  line.size - indent - content) ||
                !mediawiki_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.size >= 4u && line.data[0] == '-') {
            size_t i = 0u;
            while (i < line.size && line.data[i] == '-') ++i;
            if (i == line.size && i >= 4u) {
                if (!mediawiki_literal(parser, "\n---\n\n")) return 0;
                continue;
            }
        }
        if (!mediawiki_inline(parser, line.data, line.size) ||
            !mediawiki_literal(parser, "\n")) return 0;
    }
    if (!parser->markdown.size && !mediawiki_literal(parser, "\n")) return 0;
    return 1;
}

int qw_mediawiki_abi_version(void)
{
    return QW_MEDIAWIKI_ABI_VERSION;
}

QwMediaWikiStatus qw_mediawiki_parse_markdown(const void *data, size_t size,
                                             QwMediaWikiDocument *out_document)
{
    MediaWikiParser parser;
    if (!data || !size || !out_document)
        return QW_MEDIAWIKI_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (size > MEDIAWIKI_MAX_INPUT_BYTES)
        return QW_MEDIAWIKI_ERR_RESOURCE_LIMIT;
    memset(&parser, 0, sizeof(parser));
    parser.markdown.limit = MEDIAWIKI_MAX_MARKDOWN_BYTES + 1u;
    if (!mediawiki_translate((const uint8_t *)data, size, &parser)) {
        QwMediaWikiStatus status = parser.error == QW_STATUS_ERR_RESOURCE_LIMIT
            ? QW_MEDIAWIKI_ERR_RESOURCE_LIMIT : QW_MEDIAWIKI_ERR_OUT_OF_MEMORY;
        free(parser.markdown.data);
        return status;
    }
    out_document->markdown = parser.markdown.data;
    out_document->markdown_size = parser.markdown.size;
    out_document->omitted_media = parser.omitted_media;
    out_document->dropped_unsafe_links = parser.dropped_link;
    return QW_MEDIAWIKI_OK;
}

void qw_mediawiki_document_free(QwMediaWikiDocument *document)
{
    if (!document) return;
    free(document->markdown);
    memset(document, 0, sizeof(*document));
}

const char *qw_mediawiki_status_string(QwMediaWikiStatus status)
{
    switch (status) {
    case QW_MEDIAWIKI_OK: return "success";
    case QW_MEDIAWIKI_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_MEDIAWIKI_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_MEDIAWIKI_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    default: return "unknown MediaWiki parser status";
    }
}

#ifndef QUIREWISE_STANDALONE_MEDIAWIKI_PARSER

static int mediawiki_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t scan_size;
    size_t position = 0u;
    if (!input || !size || size > MEDIAWIKI_MAX_INPUT_BYTES) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    scan_size = size < MEDIAWIKI_PROBE_BYTES ? size : MEDIAWIKI_PROBE_BYTES;
    while (position < scan_size) {
        MediaWikiLine line;
        if (!mediawiki_read_line(data, scan_size, &position, &line)) break;
        if ((line.size >= 5u && line.data[0] == '=' &&
             mediawiki_find(line.data, line.size, "==", 1u) != SIZE_MAX) ||
            mediawiki_find(line.data, line.size, "[[", 0u) != SIZE_MAX ||
            mediawiki_find(line.data, line.size, "'''", 0u) != SIZE_MAX ||
            mediawiki_find(line.data, line.size, "<!--", 0u) != SIZE_MAX)
            return 1;
        if (line.size >= 12u && line.data[0] == '[' &&
            (mediawiki_prefix(line.data + 1u, line.size - 1u, "http://") ||
             mediawiki_prefix(line.data + 1u, line.size - 1u, "https://")))
            return 1;
    }
    return 0;
}

static int mediawiki_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    return dot && (mediawiki_equal((const uint8_t *)dot, strlen(dot), ".wiki") ||
                   mediawiki_equal((const uint8_t *)dot, strlen(dot),
                                   ".mediawiki"));
}

static int mediawiki_open_layout(const void *input, size_t input_size,
                                 QwInputFormat input_format,
                                 const QwConvertOptions *options,
                                 QwLayoutDocument *out_document,
                                 QwConvertReport *report)
{
    QwMediaWikiDocument wiki = {0};
    int omitted_media;
    int dropped_unsafe_links;
    int rc;
    (void)options;
    if (!input || !input_size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_MEDIAWIKI) return QW_STATUS_ERR_INVALID_ARGUMENT;
    {
        QwMediaWikiStatus status = qw_mediawiki_parse_markdown(
            input, input_size, &wiki);
        if (status == QW_MEDIAWIKI_ERR_INVALID_ARGUMENT)
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        if (status == QW_MEDIAWIKI_ERR_RESOURCE_LIMIT)
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        if (status != QW_MEDIAWIKI_OK) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    omitted_media = wiki.omitted_media;
    dropped_unsafe_links = wiki.dropped_unsafe_links;
    rc = qw_markdown_format_open_layout(wiki.markdown,
                                        wiki.markdown_size,
                                        QW_FORMAT_MARKDOWN, NULL,
                                        out_document, report);
    qw_mediawiki_document_free(&wiki);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        (void)snprintf(report->note, sizeof(report->note),
            "MediaWiki subset rendered through CommonMark; templates, references, tables and HTML tags are not interpreted%s%s",
            omitted_media ? "; image/file embeds became label text" : "",
            dropped_unsafe_links ? "; unsafe external links were reduced to label text" : "");
    }
    return rc;
}

const QwFormatDescriptor qw_format_mediawiki = {
    .id = QW_FORMAT_MEDIAWIKI,
    .name = "MediaWiki",
    .probe = mediawiki_probe,
    .open_layout = mediawiki_open_layout,
    .max_input_bytes = MEDIAWIKI_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};

const QwPathFormatDescriptor qw_path_format_mediawiki = {
    .id = QW_FORMAT_MEDIAWIKI,
    .probe_path = mediawiki_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};

#endif

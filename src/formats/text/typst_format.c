#ifndef QUIREWISE_STANDALONE_TYPST_PARSER
#include "../../core/format_registry.h"
#include "markdown_format.h"
#endif
#include "../../core/text_buffer.h"

#include <quirewise/typst.h>
#include "quirewise/status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TYPST_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    TYPST_MAX_MARKDOWN_BYTES = 128u * 1024u * 1024u,
    TYPST_PROBE_BYTES = 65536u
};

typedef struct TypstParser {
    QwTextBuffer markdown;
    QwTextBuffer raw_code;
    int error;
    int in_block_comment;
    int in_code_block;
    int skip_code_brace_depth;
    size_t raw_fence;
    int omitted_images;
    int skipped_code;
    int skipped_unsafe_links;
    int reduced_math;
} TypstParser;

typedef struct TypstLine {
    const uint8_t *data;
    size_t size;
} TypstLine;

static int typst_append(TypstParser *parser, const char *text, size_t size)
{
    if (parser->markdown.size > TYPST_MAX_MARKDOWN_BYTES ||
        size > TYPST_MAX_MARKDOWN_BYTES - parser->markdown.size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&parser->markdown, text, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int typst_literal(TypstParser *parser, const char *text)
{
    return typst_append(parser, text, strlen(text));
}

static int typst_line_read(const uint8_t *data, size_t size,
                           size_t *position, TypstLine *line)
{
    size_t start;
    size_t end;
    if (!data || !position || !line || *position >= size) return 0;
    start = *position;
    end = start;
    while (end < size && data[end] != '\r' && data[end] != '\n') ++end;
    line->data = data + start;
    line->size = end - start;
    if (end < size && data[end] == '\r') ++end;
    if (end < size && data[end] == '\n') ++end;
    *position = end;
    return 1;
}

static int typst_utf8_valid(const uint8_t *data, size_t size)
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

static int typst_prefix(const uint8_t *text, size_t size, const char *prefix)
{
    size_t n = strlen(prefix);
    return text && size >= n && memcmp(text, prefix, n) == 0;
}

static int typst_url_safe(const uint8_t *url, size_t size)
{
    size_t i;
    int scheme;
    if (!url || !size) return 0;
    scheme = typst_prefix(url, size, "https://") ||
                 typst_prefix(url, size, "http://") ||
                 typst_prefix(url, size, "mailto:");
    if (!scheme) return 0;
    for (i = 0u; i < size; ++i)
        if (url[i] <= 0x20u || url[i] == '"' || url[i] == '\'' ||
            url[i] == '<' || url[i] == '>') return 0;
    return 1;
}

static int typst_escape_markdown(TypstParser *parser,
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
            if (!typst_literal(parser, escaped)) return 0;
        } else if (!typst_append(parser, (const char *)text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

static size_t typst_matching(const uint8_t *text, size_t size, size_t open,
                             uint8_t left, uint8_t right)
{
    size_t i;
    unsigned depth = 0u;
    int quoted = 0;
    int escaped = 0;
    if (open >= size || text[open] != left) return SIZE_MAX;
    for (i = open; i < size; ++i) {
        uint8_t c = text[i];
        if (quoted) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') quoted = 0;
            continue;
        }
        if (c == '"') { quoted = 1; continue; }
        if (c == left) ++depth;
        else if (c == right && --depth == 0u) return i;
    }
    return SIZE_MAX;
}

static int typst_inline(TypstParser *parser, const uint8_t *text, size_t size,
                        unsigned depth);

static int typst_emit_code_span(TypstParser *parser,
                                const uint8_t *text, size_t size)
{
    size_t i;
    size_t run = 0u;
    size_t max_run = 0u;
    size_t fence;
    for (i = 0u; i < size; ++i) {
        if (text[i] == '`') {
            if (++run > max_run) max_run = run;
        } else run = 0u;
    }
    fence = max_run + 1u;
    for (i = 0u; i < fence; ++i)
        if (!typst_literal(parser, "`")) return 0;
    if (size && (text[0] == '`' || text[0] == ' ' ||
                 text[size - 1u] == '`' || text[size - 1u] == ' ') &&
        !typst_literal(parser, " ")) return 0;
    if (size && !typst_append(parser, (const char *)text, size)) return 0;
    if (size && (text[0] == '`' || text[0] == ' ' ||
                 text[size - 1u] == '`' || text[size - 1u] == ' ') &&
        !typst_literal(parser, " ")) return 0;
    for (i = 0u; i < fence; ++i)
        if (!typst_literal(parser, "`")) return 0;
    return 1;
}

static int typst_link_expression(TypstParser *parser, const uint8_t *text,
                                 size_t size, size_t start, unsigned depth,
                                 size_t *next)
{
    size_t close_paren;
    size_t close_label;
    size_t url_start = start + 6u;
    size_t url_end;
    size_t label_open;
    if (size < start + 7u || memcmp(text + start, "#link(", 6u) != 0 ||
        text[url_start] != '"') return 0;
    url_end = url_start + 1u;
    while (url_end < size && text[url_end] != '"') {
        if (text[url_end] == '\\') return 0;
        ++url_end;
    }
    if (url_end >= size || url_end + 1u >= size || text[url_end + 1u] != ')')
        return 0;
    close_paren = url_end + 1u;
    label_open = close_paren + 1u;
    if (label_open >= size || text[label_open] != '[') return 0;
    close_label = typst_matching(text, size, label_open, '[', ']');
    if (close_label == SIZE_MAX) return 0;
    if (!typst_url_safe(text + url_start + 1u, url_end - url_start - 1u)) {
        parser->skipped_unsafe_links = 1;
        if (!typst_inline(parser, text + label_open + 1u,
                          close_label - label_open - 1u, depth + 1u)) return 0;
    } else {
        if (!typst_literal(parser, "[") ||
            !typst_inline(parser, text + label_open + 1u,
                          close_label - label_open - 1u, depth + 1u) ||
            !typst_literal(parser, "](<")) return 0;
        if (!typst_append(parser, (const char *)text + url_start + 1u,
                          url_end - url_start - 1u) ||
            !typst_literal(parser, ">)")) return 0;
    }
    *next = close_label + 1u;
    return 1;
}

static int typst_inline(TypstParser *parser, const uint8_t *text, size_t size,
                        unsigned depth)
{
    size_t i = 0u;
    if (depth > 12u) return typst_escape_markdown(parser, text, size);
    while (i < size) {
        if (parser->in_block_comment) {
            size_t end = i;
            while (end + 1u < size &&
                   !(text[end] == '*' && text[end + 1u] == '/')) ++end;
            if (end + 1u >= size) return 1;
            parser->in_block_comment = 0;
            i = end + 2u;
            continue;
        }
        if (typst_prefix(text + i, size - i, "https://") ||
            typst_prefix(text + i, size - i, "http://") ||
            typst_prefix(text + i, size - i, "mailto:")) {
            size_t end = i;
            size_t link_end;
            while (end < size && text[end] > 0x20u && text[end] != '<' &&
                   text[end] != '>' && text[end] != '"' && text[end] != '\'')
                ++end;
            link_end = end;
            while (link_end > i &&
                   (text[link_end - 1u] == '.' || text[link_end - 1u] == ',' ||
                    text[link_end - 1u] == ';' || text[link_end - 1u] == ':' ||
                    text[link_end - 1u] == '!' || text[link_end - 1u] == '?'))
                --link_end;
            if (typst_url_safe(text + i, link_end - i)) {
                if (!typst_literal(parser, "[") ||
                    !typst_escape_markdown(parser, text + i, link_end - i) ||
                    !typst_literal(parser, "](<") ||
                    !typst_append(parser, (const char *)text + i,
                                  link_end - i) ||
                    !typst_literal(parser, ">)")) return 0;
                i = link_end;
                continue;
            }
        }
        if (i + 1u < size && text[i] == '/' && text[i + 1u] == '/') return 1;
        if (i + 1u < size && text[i] == '/' && text[i + 1u] == '*') {
            parser->in_block_comment = 1;
            i += 2u;
            continue;
        }
        if (text[i] == '`') {
            size_t run = 1u;
            size_t end;
            while (i + run < size && text[i + run] == '`') ++run;
            end = i + run;
            while (end < size) {
                size_t found;
                if (text[end] != '`') { ++end; continue; }
                found = end;
                while (found < size && text[found] == '`') ++found;
                if (found - end == run) break;
                end = found;
            }
            if (end < size) {
                if (!typst_emit_code_span(parser, text + i + run,
                                          end - i - run)) return 0;
                i = end + run;
                continue;
            }
        }
        if (text[i] == '#' && depth < 12u) {
            size_t next;
            {
                static const char quote_prefix[] = "#quote(block: true)[";
                size_t quote_prefix_size = sizeof(quote_prefix) - 1u;
                if (i + quote_prefix_size <= size &&
                    memcmp(text + i, quote_prefix, quote_prefix_size) == 0) {
                    size_t open = i + quote_prefix_size - 1u;
                    size_t close = typst_matching(text, size, open, '[', ']');
                    if (close != SIZE_MAX) {
                        if (!typst_literal(parser, "> ") ||
                            !typst_inline(parser, text + open + 1u,
                                          close - open - 1u, depth + 1u) ||
                            !typst_literal(parser, "\n")) return 0;
                        i = close + 1u;
                        continue;
                    }
                }
            }
            if (typst_link_expression(parser, text, size, i, depth, &next)) {
                i = next;
                continue;
            }
            if (i + 7u <= size && memcmp(text + i, "#image(", 7u) == 0) {
                size_t open = i + 6u;
                size_t close = typst_matching(text, size, open, '(', ')');
                if (close != SIZE_MAX) {
                    if (!typst_literal(parser, "[Image omitted]")) return 0;
                    parser->omitted_images = 1;
                    i = close + 1u;
                    continue;
                }
            }
            if ((i + 6u <= size && memcmp(text + i, "#emph[", 6u) == 0) ||
                (i + 8u <= size && memcmp(text + i, "#strong[", 8u) == 0)) {
                size_t marker = text[i + 1u] == 'e' ? 6u : 8u;
                size_t open = i + marker - 1u;
                size_t close = typst_matching(text, size, open, '[', ']');
                const char *style = marker == 6u ? "*" : "**";
                if (close != SIZE_MAX) {
                    if (!typst_literal(parser, style) ||
                        !typst_inline(parser, text + open + 1u,
                                      close - open - 1u, depth + 1u) ||
                        !typst_literal(parser, style)) return 0;
                    i = close + 1u;
                    continue;
                }
            }
            /* Typst code expressions are deliberately not executed. */
            parser->skipped_code = 1;
            return 1;
        }
        if (text[i] == '$') {
            size_t end = i + 1u;
            while (end < size && text[end] != '$') {
                if (text[end] == '\\' && end + 1u < size) end += 2u;
                else ++end;
            }
            if (end < size) {
                if (!typst_emit_code_span(parser, text + i + 1u,
                                          end - i - 1u)) return 0;
                parser->reduced_math = 1;
                i = end + 1u;
                continue;
            }
        }
        if (text[i] == '*' || text[i] == '_') {
            uint8_t marker = text[i];
            size_t end = i + 1u;
            while (end < size && text[end] != marker) ++end;
            if (end < size && end > i + 1u && text[i + 1u] != ' ' &&
                text[end - 1u] != ' ') {
                const char *style = marker == '*' ? "**" : "*";
                if (!typst_literal(parser, style) ||
                    !typst_inline(parser, text + i + 1u, end - i - 1u,
                                  depth + 1u) || !typst_literal(parser, style))
                    return 0;
                i = end + 1u;
                continue;
            }
        }
        if (text[i] == '<') {
            size_t end = i + 1u;
            while (end < size && text[end] != '>' && text[end] != ' ' &&
                   text[end] != '\t') ++end;
            if (end < size && text[end] == '>') {
                /* Typst labels identify nodes; they are not visible text. */
                i = end + 1u;
                continue;
            }
        }
        if (text[i] == '@') {
            size_t end = i + 1u;
            while (end < size && ((text[end] >= 'A' && text[end] <= 'Z') ||
                   (text[end] >= 'a' && text[end] <= 'z') ||
                   (text[end] >= '0' && text[end] <= '9') ||
                   text[end] == '_' || text[end] == '-' || text[end] == ':')) ++end;
            if (end > i + 1u) {
                if (!typst_escape_markdown(parser, text + i + 1u,
                                           end - i - 1u)) return 0;
                i = end;
                continue;
            }
        }
        if (!typst_escape_markdown(parser, text + i, 1u)) return 0;
        ++i;
    }
    return 1;
}

static int typst_heading(const uint8_t *text, size_t size,
                         unsigned *level, size_t *content)
{
    size_t i = 0u;
    while (i < size && text[i] == '=') ++i;
    if (!i || i >= size || text[i] != ' ') return 0;
    *level = (unsigned)(i > 6u ? 6u : i);
    *content = i;
    while (*content < size && text[*content] == ' ') ++*content;
    return 1;
}

static int typst_list(const uint8_t *text, size_t size, size_t *content,
                      int *ordered)
{
    size_t i = 0u;
    while (i < size && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i + 1u >= size || text[i + 1u] != ' ') return 0;
    if (text[i] != '-' && text[i] != '+') return 0;
    *ordered = text[i] == '+';
    i += 2u;
    while (i < size && text[i] == ' ') ++i;
    *content = i;
    return 1;
}

static int typst_raw_open(const uint8_t *text, size_t size,
                          size_t *fence, size_t *content)
{
    size_t i = 0u;
    while (i < size && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (size - i < 3u || text[i] != '`' || text[i + 1u] != '`' ||
        text[i + 2u] != '`') return 0;
    *fence = 3u;
    while (i + *fence < size && text[i + *fence] == '`') ++*fence;
    *content = i + *fence;
    return 1;
}

static int typst_raw_close(const uint8_t *text, size_t size, size_t fence)
{
    size_t i = 0u;
    size_t run = 0u;
    while (i < size && (text[i] == ' ' || text[i] == '\t')) ++i;
    while (i < size && text[i] == '`') { ++run; ++i; }
    while (i < size && (text[i] == ' ' || text[i] == '\t')) ++i;
    return run == fence && i == size;
}

static int typst_code_fence(TypstParser *parser,
                            const QwTextBuffer *code)
{
    size_t i;
    size_t run = 0u;
    size_t max_run = 0u;
    size_t fence;
    for (i = 0u; i < code->size; ++i) {
        if (code->data[i] == '`') {
            if (++run > max_run) max_run = run;
        } else run = 0u;
    }
    fence = max_run < 2u ? 3u : max_run + 1u;
    for (i = 0u; i < fence; ++i)
        if (!typst_literal(parser, "`")) return 0;
    if (!typst_literal(parser, "\n") ||
        (code->size && !typst_append(parser, code->data, code->size))) return 0;
    if (code->size && code->data[code->size - 1u] != '\n' &&
        !typst_literal(parser, "\n")) return 0;
    for (i = 0u; i < fence; ++i)
        if (!typst_literal(parser, "`")) return 0;
    return typst_literal(parser, "\n\n");
}

static void typst_count_braces(const uint8_t *text, size_t size, int *depth)
{
    size_t i;
    int quoted = 0;
    int escaped = 0;
    for (i = 0u; i < size; ++i) {
        uint8_t c = text[i];
        if (!quoted && i + 1u < size && c == '/' && text[i + 1u] == '/') break;
        if (quoted) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') quoted = 0;
        } else if (c == '"') {
            quoted = 1;
        } else if (c == '{') {
            ++*depth;
        } else if (c == '}' && *depth > 0) {
            --*depth;
        }
    }
}

static int typst_supported_expression_line(const uint8_t *text, size_t size)
{
    static const char quote_prefix[] = "#quote(block: true)[";
    return typst_prefix(text, size, "#link(") ||
           typst_prefix(text, size, "#image(") ||
           typst_prefix(text, size, "#emph[") ||
           typst_prefix(text, size, "#strong[") ||
           typst_prefix(text, size, quote_prefix);
}

static int typst_translate(const uint8_t *data, size_t size,
                           TypstParser *parser)
{
    size_t position = 0u;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    while (position < size) {
        TypstLine line;
        size_t left = 0u;
        size_t right;
        size_t heading_content;
        size_t raw_fence;
        size_t raw_content;
        unsigned heading_level;
        int ordered;
        if (!typst_line_read(data, size, &position, &line)) break;
        while (left < line.size && (line.data[left] == ' ' ||
                                    line.data[left] == '\t')) ++left;
        right = line.size;
        while (right > left && (line.data[right - 1u] == ' ' ||
                                line.data[right - 1u] == '\t')) --right;
        if (parser->skip_code_brace_depth > 0) {
            typst_count_braces(line.data + left, right - left,
                               &parser->skip_code_brace_depth);
            continue;
        }
        if (parser->in_code_block) {
            if (typst_raw_close(line.data + left, right - left,
                                parser->raw_fence)) {
                if (!typst_code_fence(parser, &parser->raw_code)) return 0;
                free(parser->raw_code.data);
                memset(&parser->raw_code, 0, sizeof(parser->raw_code));
                parser->in_code_block = 0;
            } else if (!qw_text_buffer_append(&parser->raw_code,
                                              (const char *)line.data,
                                              line.size) ||
                       !qw_text_buffer_append(&parser->raw_code, "\n", 1u)) {
                parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
                return 0;
            }
            continue;
        }
        if (typst_raw_open(line.data, line.size, &raw_fence, &raw_content)) {
            parser->in_code_block = 1;
            parser->raw_fence = raw_fence;
            parser->raw_code.limit = TYPST_MAX_INPUT_BYTES + 1u;
            (void)raw_content; /* Optional raw language tags are not interpreted. */
            continue;
        }
        if (left == right) {
            if (!typst_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.data[left] == '#' &&
            !typst_supported_expression_line(line.data + left, right - left)) {
            /* set/show/import/include and all other script lines are not run. */
            parser->skipped_code = 1;
            typst_count_braces(line.data + left, right - left,
                               &parser->skip_code_brace_depth);
            continue;
        }
        if (typst_heading(line.data + left, right - left,
                          &heading_level, &heading_content)) {
            size_t i;
            for (i = 0u; i < heading_level; ++i)
                if (!typst_literal(parser, "#")) return 0;
            if (!typst_literal(parser, " ") ||
                !typst_inline(parser, line.data + left + heading_content,
                              right - left - heading_content, 0u) ||
                !typst_literal(parser, "\n\n")) return 0;
            continue;
        }
        if (typst_list(line.data + left, right - left,
                       &heading_content, &ordered)) {
            if (!typst_literal(parser, ordered ? "1. " : "- ") ||
                !typst_inline(parser, line.data + left + heading_content,
                              right - left - heading_content, 0u) ||
                !typst_literal(parser, "\n")) return 0;
            continue;
        }
        if (line.data[left] == '/' && left + 1u < right &&
            line.data[left + 1u] == ' ') {
            size_t colon = left + 2u;
            while (colon < right && line.data[colon] != ':') ++colon;
            if (colon < right && colon + 1u < right && line.data[colon + 1u] == ' ') {
                if (!typst_literal(parser, "**") ||
                    !typst_inline(parser, line.data + left + 2u,
                                  colon - left - 2u, 0u) ||
                    !typst_literal(parser, "** — ") ||
                    !typst_inline(parser, line.data + colon + 2u,
                                  right - colon - 2u, 0u) ||
                    !typst_literal(parser, "\n\n")) return 0;
                continue;
            }
        }
        if (!typst_inline(parser, line.data + left, right - left, 0u) ||
            !typst_literal(parser, "\n")) return 0;
    }
    if (parser->in_code_block == 1) {
        if (!typst_code_fence(parser, &parser->raw_code)) return 0;
        parser->in_code_block = 0;
        free(parser->raw_code.data);
        memset(&parser->raw_code, 0, sizeof(parser->raw_code));
    }
    return 1;
}

int qw_typst_abi_version(void)
{
    return QW_TYPST_ABI_VERSION;
}

QwTypstStatus qw_typst_parse_markdown(const void *data, size_t size,
                                     QwTypstDocument *out_document)
{
    TypstParser parser;
    if (!data || !size || !out_document) return QW_TYPST_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (size > TYPST_MAX_INPUT_BYTES) return QW_TYPST_ERR_RESOURCE_LIMIT;
    if (!typst_utf8_valid((const uint8_t *)data, size))
        return QW_TYPST_ERR_INVALID_UTF8;
    memset(&parser, 0, sizeof(parser));
    parser.markdown.limit = TYPST_MAX_MARKDOWN_BYTES + 1u;
    if (!typst_translate((const uint8_t *)data, size, &parser) ||
        (!parser.markdown.size && !typst_literal(&parser, "\n"))) {
        QwTypstStatus status = parser.error == QW_STATUS_ERR_RESOURCE_LIMIT
            ? QW_TYPST_ERR_RESOURCE_LIMIT : QW_TYPST_ERR_OUT_OF_MEMORY;
        free(parser.markdown.data);
        free(parser.raw_code.data);
        return status;
    }
    free(parser.raw_code.data);
    out_document->markdown = parser.markdown.data;
    out_document->markdown_size = parser.markdown.size;
    out_document->omitted_images = parser.omitted_images;
    out_document->skipped_code_expressions = parser.skipped_code;
    out_document->dropped_unsafe_links = parser.skipped_unsafe_links;
    out_document->reduced_math = parser.reduced_math;
    return QW_TYPST_OK;
}

void qw_typst_document_free(QwTypstDocument *document)
{
    if (!document) return;
    free(document->markdown);
    memset(document, 0, sizeof(*document));
}

const char *qw_typst_status_string(QwTypstStatus status)
{
    switch (status) {
    case QW_TYPST_OK: return "success";
    case QW_TYPST_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_TYPST_ERR_INVALID_UTF8: return "input is not valid UTF-8";
    case QW_TYPST_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_TYPST_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    default: return "unknown Typst parser status";
    }
}

#ifndef QUIREWISE_STANDALONE_TYPST_PARSER

static int typst_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t position = 0u;
    size_t scan_size;
    if (!input || !size || size > TYPST_MAX_INPUT_BYTES) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    scan_size = size < TYPST_PROBE_BYTES ? size : TYPST_PROBE_BYTES;
    if (!typst_utf8_valid(data + position, scan_size - position)) return 0;
    while (position < scan_size) {
        TypstLine line;
        size_t i = 0u;
        if (!typst_line_read(data, scan_size, &position, &line)) break;
        while (i < line.size && (line.data[i] == ' ' || line.data[i] == '\t')) ++i;
        if (line.size - i >= 2u && line.data[i] == '=' && line.data[i + 1u] == ' ')
            return 1;
        if (line.size - i >= 3u && line.data[i] == '`' &&
            line.data[i + 1u] == '`' && line.data[i + 2u] == '`') return 1;
        if (typst_prefix(line.data + i, line.size - i, "#set ") ||
            typst_prefix(line.data + i, line.size - i, "#show ") ||
            typst_prefix(line.data + i, line.size - i, "#let ") ||
            typst_prefix(line.data + i, line.size - i, "#import ") ||
            typst_prefix(line.data + i, line.size - i, "#include ") ||
            typst_prefix(line.data + i, line.size - i, "#image(") ||
            typst_prefix(line.data + i, line.size - i, "#link(")) return 1;
    }
    return 0;
}

static int typst_path_probe(const char *path)
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
    if (strlen(dot) == 4u) {
        return (dot[1] == 't' || dot[1] == 'T') &&
               (dot[2] == 'y' || dot[2] == 'Y') &&
               (dot[3] == 'p' || dot[3] == 'P');
    }
    if (strlen(dot) == 6u) {
        static const char extension[] = ".typst";
        size_t i;
        for (i = 0u; i < sizeof(extension) - 1u; ++i) {
            char actual = dot[i];
            if (actual >= 'A' && actual <= 'Z') actual = (char)(actual + 32);
            if (actual != extension[i]) return 0;
        }
        return 1;
    }
    return 0;
}

static int typst_open_layout(const void *input, size_t input_size,
                             QwInputFormat input_format,
                             const QwConvertOptions *options,
                             QwLayoutDocument *out_document,
                             QwConvertReport *report)
{
    QwTypstDocument typst = {0};
    int rc;
    (void)options;
    if (!input || !input_size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_TYP) return QW_STATUS_ERR_INVALID_ARGUMENT;
    {
        QwTypstStatus status = qw_typst_parse_markdown(input, input_size,
                                                       &typst);
        if (status == QW_TYPST_ERR_INVALID_ARGUMENT)
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        if (status == QW_TYPST_ERR_INVALID_UTF8) return QW_STATUS_ERR_DECODE;
        if (status == QW_TYPST_ERR_RESOURCE_LIMIT) return QW_STATUS_ERR_RESOURCE_LIMIT;
        if (status != QW_TYPST_OK) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    rc = qw_markdown_format_open_layout(typst.markdown,
                                        typst.markdown_size,
                                        QW_FORMAT_MARKDOWN, NULL,
                                        out_document, report);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        (void)snprintf(report->note, sizeof(report->note),
            "Typst markup subset rendered through CommonMark; code expressions, imports and includes are not executed%s%s%s",
            typst.omitted_images ? "; images were replaced by text" : "",
            typst.dropped_unsafe_links ? "; unsafe links were reduced to label text" : "",
            typst.reduced_math ? "; math expressions were retained as plain code text" : "");
    }
    qw_typst_document_free(&typst);
    return rc;
}

const QwFormatDescriptor qw_format_typ = {
    .id = QW_FORMAT_TYP,
    .name = "Typst",
    .probe = typst_probe,
    .open_layout = typst_open_layout,
    .max_input_bytes = TYPST_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};

const QwPathFormatDescriptor qw_path_format_typ = {
    .id = QW_FORMAT_TYP,
    .probe_path = typst_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};

#endif

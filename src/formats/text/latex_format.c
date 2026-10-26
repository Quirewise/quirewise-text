#ifndef QUIREWISE_STANDALONE_LATEX_PARSER
#include "../../core/format_registry.h"
#include "markdown_format.h"
#endif
#include "../../core/text_buffer.h"

#include <quirewise/latex.h>
#include "quirewise/status.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LATEX_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    LATEX_MAX_MARKDOWN_BYTES = 128u * 1024u * 1024u,
    LATEX_PROBE_BYTES = 65536u,
    LATEX_MAX_LIST_DEPTH = 16u,
    LATEX_MAX_ENV_NAME = 32u
};

typedef struct LatexParser {
    QwTextBuffer markdown;
    QwTextBuffer code;
    QwTextBuffer title;
    int error;
    int has_document_environment;
    int in_document;
    int in_code_environment;
    int skip_environment;
    int quote_depth;
    int list_depth;
    unsigned list_overflow_depth;
    int list_ordered[LATEX_MAX_LIST_DEPTH];
    int skipped_commands;
    int omitted_images;
    int omitted_tables;
    int omitted_figures;
    int reduced_math;
    int skipped_unsafe_links;
    int title_emitted;
    char environment[LATEX_MAX_ENV_NAME];
} LatexParser;

typedef struct LatexLine {
    const uint8_t *data;
    size_t size;
} LatexLine;

static int latex_append(LatexParser *parser, const char *text, size_t size)
{
    if (parser->markdown.size > LATEX_MAX_MARKDOWN_BYTES ||
        size > LATEX_MAX_MARKDOWN_BYTES - parser->markdown.size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&parser->markdown, text, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int latex_literal(LatexParser *parser, const char *text)
{
    return latex_append(parser, text, strlen(text));
}

static int latex_line_read(const uint8_t *data, size_t size,
                           size_t *position, LatexLine *line)
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

static int latex_utf8_valid(const uint8_t *data, size_t size)
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

static int latex_equal(const uint8_t *text, size_t size, const char *literal)
{
    size_t i;
    size_t n = strlen(literal);
    if (size != n) return 0;
    for (i = 0u; i < n; ++i) {
        uint8_t c = text[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32u);
        if (c != (uint8_t)literal[i]) return 0;
    }
    return 1;
}

static int latex_prefix(const uint8_t *text, size_t size, const char *prefix)
{
    return text && size >= strlen(prefix) &&
           memcmp(text, prefix, strlen(prefix)) == 0;
}

static int latex_prefix_ascii_equal(const uint8_t *text, size_t size,
                                    const char *prefix)
{
    size_t i;
    size_t n = strlen(prefix);
    if (!text || size < n) return 0;
    for (i = 0u; i < n; ++i) {
        uint8_t c = text[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + ('a' - 'A'));
        if (c != (uint8_t)prefix[i]) return 0;
    }
    return 1;
}

static size_t latex_find(const uint8_t *text, size_t size,
                         const char *needle, size_t start)
{
    size_t i;
    size_t n = strlen(needle);
    if (!text || !needle || !n || start > size || n > size - start)
        return SIZE_MAX;
    for (i = start; i <= size - n; ++i)
        if (memcmp(text + i, needle, n) == 0) return i;
    return SIZE_MAX;
}

static size_t latex_matching(const uint8_t *text, size_t size, size_t open,
                             uint8_t left, uint8_t right)
{
    size_t i;
    unsigned depth = 0u;
    int escaped = 0;
    if (!text || open >= size || text[open] != left) return SIZE_MAX;
    for (i = open; i < size; ++i) {
        uint8_t c = text[i];
        if (escaped) { escaped = 0; continue; }
        if (c == '\\') { escaped = 1; continue; }
        if (c == '%') {
            while (i < size && text[i] != '\n' && text[i] != '\r') ++i;
            continue;
        }
        if (c == left) ++depth;
        else if (c == right && depth && --depth == 0u) return i;
    }
    return SIZE_MAX;
}

static int latex_next_group(const uint8_t *text, size_t size, size_t position,
                            uint8_t open, uint8_t close,
                            size_t *content_start, size_t *content_size,
                            size_t *after)
{
    size_t end;
    while (position < size && (text[position] == ' ' || text[position] == '\t'))
        ++position;
    if (position >= size || text[position] != open) return 0;
    end = latex_matching(text, size, position, open, close);
    if (end == SIZE_MAX) return 0;
    if (content_start) *content_start = position + 1u;
    if (content_size) *content_size = end - position - 1u;
    if (after) *after = end + 1u;
    return 1;
}

static int latex_escape_markdown(LatexParser *parser,
                                 const uint8_t *text, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        const char *replacement = NULL;
        switch (text[i]) {
        case '\\': replacement = "\\\\"; break;
        case '`': replacement = "\\`"; break;
        case '*': replacement = "\\*"; break;
        case '_': replacement = "\\_"; break;
        case '[': replacement = "\\["; break;
        case ']': replacement = "\\]"; break;
        case '#': replacement = "\\#"; break;
        case '~': replacement = "\\~"; break;
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        default: break;
        }
        if (replacement) {
            if (!latex_literal(parser, replacement)) return 0;
        } else if (!latex_append(parser, (const char *)text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

static int latex_url_safe(const uint8_t *url, size_t size)
{
    size_t i;
    size_t authority_start;
    int has_authority;
    if (!url || !size) return 0;
    if (latex_prefix_ascii_equal(url, size, "https://")) authority_start = 8u;
    else if (latex_prefix_ascii_equal(url, size, "http://")) authority_start = 7u;
    else if (latex_prefix_ascii_equal(url, size, "mailto:")) authority_start = 7u;
    else return 0;
    has_authority = authority_start < size && url[authority_start] != '/' &&
                    url[authority_start] != '?' && url[authority_start] != '#';
    if (!has_authority) return 0;
    for (i = 0u; i < size; ++i)
        if (url[i] <= 0x20u || url[i] == '"' || url[i] == '\'' ||
            url[i] == '<' || url[i] == '>' || url[i] == '{' ||
            url[i] == '}' || url[i] == '\\') return 0;
    return 1;
}

static int latex_emit_code(LatexParser *parser,
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
        if (!latex_literal(parser, "`")) return 0;
    if (size && (text[0] == '`' || text[0] == ' ' ||
                 text[size - 1u] == '`' || text[size - 1u] == ' ') &&
        !latex_literal(parser, " ")) return 0;
    if (size && !latex_append(parser, (const char *)text, size)) return 0;
    if (size && (text[0] == '`' || text[0] == ' ' ||
                 text[size - 1u] == '`' || text[size - 1u] == ' ') &&
        !latex_literal(parser, " ")) return 0;
    for (i = 0u; i < fence; ++i)
        if (!latex_literal(parser, "`")) return 0;
    return 1;
}

static int latex_inline(LatexParser *parser, const uint8_t *text, size_t size,
                        unsigned depth);

static int latex_inline(LatexParser *parser, const uint8_t *text, size_t size,
                        unsigned depth)
{
    size_t i = 0u;
    if (depth > 16u) return latex_escape_markdown(parser, text, size);
    while (i < size) {
        if (text[i] == '%') return 1;
        if (text[i] == '{' || text[i] == '}') { ++i; continue; }
        if (text[i] == '$') {
            size_t end = i + 1u;
            while (end < size) {
                if (text[end] == '\\' && end + 1u < size) { end += 2u; continue; }
                if (text[end] == '$') break;
                ++end;
            }
            if (end < size) {
                if (!latex_literal(parser, "[Math: ") ||
                    !latex_escape_markdown(parser, text + i + 1u,
                                           end - i - 1u) ||
                    !latex_literal(parser, "]")) return 0;
                parser->reduced_math = 1;
                i = end + 1u;
                continue;
            }
        }
        if (text[i] == '\\') {
            size_t name_start = i + 1u;
            size_t name_end = name_start;
            size_t after;
            size_t arg_start;
            size_t arg_size;
            int starred = 0;
            if (name_start >= size) break;
            if (text[name_start] == '\\') {
                if (!latex_literal(parser, "  \n")) return 0;
                i += 2u;
                continue;
            }
            if ((text[name_start] >= 'A' && text[name_start] <= 'Z') ||
                (text[name_start] >= 'a' && text[name_start] <= 'z')) {
                while (name_end < size &&
                       ((text[name_end] >= 'A' && text[name_end] <= 'Z') ||
                        (text[name_end] >= 'a' && text[name_end] <= 'z')))
                    ++name_end;
                if (name_end < size && text[name_end] == '*') {
                    starred = 1;
                    ++name_end;
                }
            } else {
                name_end = name_start + 1u;
            }
            {
                size_t name_size = name_end - name_start - (starred ? 1u : 0u);
                const uint8_t *name = text + name_start;
                if ((name_size == 1u && (name[0] == '%' || name[0] == '$' ||
                                         name[0] == '&' || name[0] == '#' ||
                                         name[0] == '_' || name[0] == '{' ||
                                         name[0] == '}')) ||
                    (name_size == 12u && memcmp(name, "textbackslash", 12u) == 0)) {
                    uint8_t literal = name_size == 12u ? '\\' : name[0];
                    if (!latex_escape_markdown(parser, &literal, 1u)) return 0;
                    i = name_end;
                    if (name_size == 12u && i < size && text[i] == '{') {
                        size_t empty_end = latex_matching(text, size, i, '{', '}');
                        if (empty_end != SIZE_MAX) i = empty_end + 1u;
                    }
                    continue;
                }
                if (name_size == 5u && memcmp(name, "LaTeX", 5u) == 0) {
                    if (!latex_literal(parser, "LaTeX")) return 0;
                    i = name_end;
                    continue;
                }
                if (name_size == 3u && memcmp(name, "TeX", 3u) == 0) {
                    if (!latex_literal(parser, "TeX")) return 0;
                    i = name_end;
                    continue;
                }
                if (name_size == 5u && memcmp(name, "ldots", 5u) == 0) {
                    if (!latex_literal(parser, "…")) return 0;
                    i = name_end;
                    continue;
                }
                if (name_size == 1u && (name[0] == ',' || name[0] == ';' ||
                    name[0] == ':' || name[0] == '!')) {
                    if (!latex_literal(parser, " ")) return 0;
                    i = name_end;
                    continue;
                }
                if ((name_size == 4u && memcmp(name, "quad", 4u) == 0) ||
                    (name_size == 5u && memcmp(name, "qquad", 5u) == 0) ||
                    (name_size == 3u && memcmp(name, "par", 3u) == 0) ||
                    (name_size == 7u && memcmp(name, "newline", 7u) == 0)) {
                    if (!latex_literal(parser,
                        name_size == 3u ? "\n\n" : "\n")) return 0;
                    i = name_end;
                    continue;
                }
                if (name_size == 4u && memcmp(name, "verb", 4u) == 0) {
                    size_t delimiter = name_end;
                    size_t end;
                    if (delimiter < size && text[delimiter] == '*') ++delimiter;
                    if (delimiter < size) {
                        uint8_t mark = text[delimiter];
                        end = delimiter + 1u;
                        while (end < size && text[end] != mark) ++end;
                        if (end < size) {
                            if (!latex_emit_code(parser, text + delimiter + 1u,
                                                 end - delimiter - 1u)) return 0;
                            i = end + 1u;
                            continue;
                        }
                    }
                }
                if (name_size == 4u && memcmp(name, "href", 4u) == 0 &&
                    latex_next_group(text, size, name_end, '{', '}',
                                     &arg_start, &arg_size, &after)) {
                    size_t url_start = arg_start;
                    size_t url_size = arg_size;
                    size_t label_start;
                    size_t label_size;
                    if (!latex_next_group(text, size, after, '{', '}',
                                          &label_start, &label_size, &after))
                        return 0;
                    if (latex_url_safe(text + url_start, url_size)) {
                        if (!latex_literal(parser, "[") ||
                            !latex_inline(parser, text + label_start, label_size,
                                          depth + 1u) ||
                            !latex_literal(parser, "](<") ||
                            !latex_append(parser, (const char *)text + url_start,
                                          url_size) ||
                            !latex_literal(parser, ">)")) return 0;
                    } else {
                        parser->skipped_unsafe_links = 1;
                        if (!latex_inline(parser, text + label_start, label_size,
                                          depth + 1u)) return 0;
                    }
                    i = after;
                    continue;
                }
                if ((name_size == 4u && memcmp(name, "emph", 4u) == 0) ||
                    (name_size == 6u && memcmp(name, "textit", 6u) == 0) ||
                    (name_size == 6u && memcmp(name, "textbf", 6u) == 0) ||
                    (name_size == 6u && memcmp(name, "texttt", 6u) == 0) ||
                    (name_size == 9u && memcmp(name, "underline", 9u) == 0)) {
                    const char *style = NULL;
                    int code_style = 0;
                    if (name_size == 6u && memcmp(name, "textbf", 6u) == 0)
                        style = "**";
                    else if (name_size == 6u && memcmp(name, "texttt", 6u) == 0) {
                        style = "`";
                        code_style = 1;
                    } else if (name_size == 4u && memcmp(name, "emph", 4u) == 0)
                        style = "*";
                    else if (name_size == 6u && memcmp(name, "textit", 6u) == 0)
                        style = "*";
                    if (latex_next_group(text, size, name_end, '{', '}',
                                         &arg_start, &arg_size, &after)) {
                        if (code_style) {
                            if (!latex_emit_code(parser, text + arg_start,
                                                 arg_size)) return 0;
                        } else if (style && (!latex_literal(parser, style) ||
                                   !latex_inline(parser, text + arg_start,
                                                 arg_size, depth + 1u) ||
                                   !latex_literal(parser, style))) return 0;
                        else if (!style && !latex_inline(parser, text + arg_start,
                                                        arg_size, depth + 1u)) return 0;
                        i = after;
                        continue;
                    }
                }
                if (name_size == 3u && memcmp(name, "url", 3u) == 0 &&
                    latex_next_group(text, size, name_end, '{', '}',
                                     &arg_start, &arg_size, &after)) {
                    if (latex_url_safe(text + arg_start, arg_size)) {
                        if (!latex_literal(parser, "[") ||
                            !latex_escape_markdown(parser, text + arg_start,
                                                   arg_size) ||
                            !latex_literal(parser, "](<") ||
                            !latex_append(parser, (const char *)text + arg_start,
                                          arg_size) ||
                            !latex_literal(parser, ">)")) return 0;
                    } else {
                        parser->skipped_unsafe_links = 1;
                        if (!latex_escape_markdown(parser, text + arg_start,
                                                   arg_size)) return 0;
                    }
                    i = after;
                    continue;
                }
                if (name_size == 14u && memcmp(name, "includegraphics", 14u) == 0) {
                    size_t opt_end = name_end;
                    if (opt_end < size && text[opt_end] == '[') {
                        size_t close = latex_matching(text, size, opt_end, '[', ']');
                        if (close != SIZE_MAX) opt_end = close + 1u;
                    }
                    if (latex_next_group(text, size, opt_end, '{', '}',
                                         NULL, NULL, &after)) {
                        if (!latex_literal(parser, "[Image omitted]")) return 0;
                        parser->omitted_images = 1;
                        i = after;
                        continue;
                    }
                }
                if ((name_size == 3u && memcmp(name, "ref", 3u) == 0) ||
                    (name_size == 6u && memcmp(name, "pageref", 6u) == 0) ||
                    (name_size == 5u && (memcmp(name, "eqref", 5u) == 0 ||
                                          memcmp(name, "citep", 5u) == 0 ||
                                          memcmp(name, "citet", 5u) == 0)) ||
                    (name_size == 4u && (memcmp(name, "cite", 4u) == 0 ||
                                          memcmp(name, "Cite", 4u) == 0)) ||
                    (name_size == 7u && memcmp(name, "autoref", 7u) == 0)) {
                    if (latex_next_group(text, size, name_end, '{', '}',
                                         &arg_start, &arg_size, &after)) {
                        if (!latex_inline(parser, text + arg_start, arg_size,
                                          depth + 1u)) return 0;
                        i = after;
                        continue;
                    }
                }
                if ((name_size == 5u && memcmp(name, "input", 5u) == 0) ||
                    (name_size == 7u && (memcmp(name, "include", 7u) == 0 ||
                                          memcmp(name, "subfile", 7u) == 0)) ||
                    (name_size == 5u && memcmp(name, "label", 5u) == 0)) {
                    if (latex_next_group(text, size, name_end, '{', '}',
                                         NULL, NULL, &after)) {
                        parser->skipped_commands = 1;
                        i = after;
                        continue;
                    }
                }
                if ((name_size == 10u && memcmp(name, "usepackage", 10u) == 0) ||
                    (name_size == 12u && memcmp(name, "documentclass", 12u) == 0)) {
                    size_t skip = name_end;
                    if (skip < size && text[skip] == '[') {
                        size_t end = latex_matching(text, size, skip, '[', ']');
                        if (end != SIZE_MAX) skip = end + 1u;
                    }
                    if (latex_next_group(text, size, skip, '{', '}',
                                         NULL, NULL, &after)) {
                        parser->skipped_commands = 1;
                        i = after;
                        continue;
                    }
                }
                if ((name_size == 3u && memcmp(name, "def", 3u) == 0) ||
                    (name_size == 4u && (memcmp(name, "read", 4u) == 0 ||
                                          memcmp(name, "gdef", 4u) == 0 ||
                                          memcmp(name, "edef", 4u) == 0 ||
                                          memcmp(name, "xdef", 4u) == 0)) ||
                    (name_size == 5u && memcmp(name, "write", 5u) == 0) ||
                    (name_size == 7u && (memcmp(name, "openout", 7u) == 0 ||
                                          memcmp(name, "catcode", 7u) == 0)) ||
                    (name_size == 10u && memcmp(name, "newcommand", 10u) == 0) ||
                    (name_size == 12u && memcmp(name, "renewcommand", 12u) == 0)) {
                    /* Never interpret definitions, file I/O, or shell escapes. */
                    parser->skipped_commands = 1;
                    return 1;
                }
                if (name_size == 1u && name[0] == '(') {
                    size_t end = latex_find(text, size, "\\)", name_end);
                    if (end != SIZE_MAX) {
                        if (!latex_literal(parser, "[Math: ") ||
                            !latex_escape_markdown(parser, text + name_end,
                                                   end - name_end) ||
                            !latex_literal(parser, "]")) return 0;
                        parser->reduced_math = 1;
                        i = end + 2u;
                        continue;
                    }
                }
                if (name_size == 1u && name[0] == '[') {
                    size_t end = latex_find(text, size, "\\]", name_end);
                    if (end != SIZE_MAX) {
                        if (!latex_literal(parser, "[Math: ") ||
                            !latex_escape_markdown(parser, text + name_end,
                                                   end - name_end) ||
                            !latex_literal(parser, "]")) return 0;
                        parser->reduced_math = 1;
                        i = end + 2u;
                        continue;
                    }
                }
                if (name_size == 9u && memcmp(name, "maketitle", 9u) == 0 &&
                    !parser->title_emitted && parser->title.size) {
                    if (!latex_literal(parser, "# ") ||
                        !latex_inline(parser, (const uint8_t *)parser->title.data,
                                      parser->title.size, depth + 1u) ||
                        !latex_literal(parser, "\n\n")) return 0;
                    parser->title_emitted = 1;
                    i = name_end;
                    continue;
                }
                parser->skipped_commands = 1;
                i = name_end;
                continue;
            }
        }
        if (text[i] == '~') {
            if (!latex_literal(parser, " ")) return 0;
            ++i;
            continue;
        }
        if (!latex_escape_markdown(parser, text + i, 1u)) return 0;
        ++i;
    }
    return 1;
}

static int latex_environment_command(const uint8_t *text, size_t size,
                                     int *is_begin, char *name,
                                     size_t name_capacity, size_t *after)
{
    size_t start;
    size_t env_start;
    size_t env_size;
    size_t end;
    if (size < 8u || text[0] != '\\') return 0;
    if (latex_prefix(text, size, "\\begin{")) *is_begin = 1;
    else if (latex_prefix(text, size, "\\end{")) *is_begin = 0;
    else return 0;
    start = 6u;
    end = latex_matching(text, size, start, '{', '}');
    if (end == SIZE_MAX) return 0;
    env_start = start + 1u;
    env_size = end - env_start;
    if (!env_size || env_size >= name_capacity) return 0;
    memcpy(name, text + env_start, env_size);
    name[env_size] = '\0';
    if (after) *after = end + 1u;
    return 1;
}

static size_t latex_find_command(const uint8_t *data, size_t size,
                                 const char *command, size_t start)
{
    size_t i = start;
    size_t command_size = strlen(command);
    if (!data || !command || start > size) return SIZE_MAX;
    while (i < size) {
        if (data[i] == '%') {
            while (i < size && data[i] != '\n' && data[i] != '\r') ++i;
            continue;
        }
        if (data[i] != '\\') {
            ++i;
            continue;
        }
        if (command_size <= size - i &&
            memcmp(data + i, command, command_size) == 0) return i;
        if (i + 1u >= size) break;
        if (!((data[i + 1u] >= 'A' && data[i + 1u] <= 'Z') ||
              (data[i + 1u] >= 'a' && data[i + 1u] <= 'z'))) {
            i += 2u;
            continue;
        }
        {
            size_t name_start = i + 1u;
            size_t name_end = name_start;
            while (name_end < size &&
                   ((data[name_end] >= 'A' && data[name_end] <= 'Z') ||
                    (data[name_end] >= 'a' && data[name_end] <= 'z')))
                ++name_end;
            if (name_end - name_start == 4u &&
                memcmp(data + name_start, "verb", 4u) == 0) {
                size_t delimiter = name_end;
                size_t line_end = delimiter;
                if (delimiter < size && data[delimiter] == '*') ++delimiter;
                if (delimiter < size) {
                    uint8_t mark = data[delimiter++];
                    line_end = delimiter;
                    while (line_end < size && data[line_end] != '\r' &&
                           data[line_end] != '\n' && data[line_end] != mark)
                        ++line_end;
                    if (line_end < size && data[line_end] == mark)
                        i = line_end + 1u;
                    else i = line_end;
                    continue;
                }
            }
            i = name_end;
        }
    }
    return SIZE_MAX;
}

static int latex_section(const uint8_t *text, size_t size,
                         unsigned *level, size_t *title_start,
                         size_t *title_size, size_t *after)
{
    static const struct { const char *name; unsigned level; } commands[] = {
        { "\\part", 1u }, { "\\chapter", 1u },
        { "\\section", 1u }, { "\\subsection", 2u },
        { "\\subsubsection", 3u }, { "\\paragraph", 4u },
        { "\\subparagraph", 5u }
    };
    size_t i;
    for (i = 0u; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        size_t n = strlen(commands[i].name);
        size_t p = n;
        size_t optional_end;
        if (size < n || memcmp(text, commands[i].name, n) != 0) continue;
        if (p < size && text[p] == '*') ++p;
        while (p < size && (text[p] == ' ' || text[p] == '\t')) ++p;
        if (p < size && text[p] == '[') {
            optional_end = latex_matching(text, size, p, '[', ']');
            if (optional_end == SIZE_MAX) continue;
            p = optional_end + 1u;
        }
        if (!latex_next_group(text, size, p, '{', '}', title_start,
                              title_size, after)) continue;
        *level = commands[i].level;
        return 1;
    }
    return 0;
}

static int latex_is_code_environment(const char *name)
{
    return strcmp(name, "verbatim") == 0 || strcmp(name, "Verbatim") == 0 ||
           strcmp(name, "lstlisting") == 0 || strcmp(name, "minted") == 0;
}

static int latex_is_math_environment(const char *name)
{
    return strcmp(name, "equation") == 0 || strcmp(name, "equation*") == 0 ||
           strcmp(name, "align") == 0 || strcmp(name, "align*") == 0 ||
           strcmp(name, "gather") == 0 || strcmp(name, "gather*") == 0 ||
           strcmp(name, "multline") == 0 || strcmp(name, "multline*") == 0 ||
           strcmp(name, "displaymath") == 0 || strcmp(name, "math") == 0;
}

static int latex_is_table_environment(const char *name)
{
    return strcmp(name, "tabular") == 0 || strcmp(name, "tabular*") == 0 ||
           strcmp(name, "longtable") == 0 || strcmp(name, "array") == 0;
}

static int latex_begin_environment(LatexParser *parser, const char *name)
{
    if (latex_is_code_environment(name)) {
        parser->in_code_environment = 1;
        (void)snprintf(parser->environment, sizeof(parser->environment), "%s", name);
        parser->code.limit = LATEX_MAX_MARKDOWN_BYTES + 1u;
        return 1;
    }
    if (latex_is_math_environment(name)) {
        parser->skip_environment = 1;
        parser->reduced_math = 1;
        if (!latex_literal(parser, "[Math omitted]\n\n")) return 0;
        (void)snprintf(parser->environment, sizeof(parser->environment), "%s", name);
        return 1;
    }
    if (latex_is_table_environment(name)) {
        parser->skip_environment = 1;
        parser->omitted_tables = 1;
        if (!latex_literal(parser, "[Table omitted]\n\n")) return 0;
        (void)snprintf(parser->environment, sizeof(parser->environment), "%s", name);
        return 1;
    }
    if (strcmp(name, "figure") == 0 || strcmp(name, "figure*") == 0) {
        parser->skip_environment = 1;
        parser->omitted_figures = 1;
        if (!latex_literal(parser, "[Figure omitted]\n\n")) return 0;
        (void)snprintf(parser->environment, sizeof(parser->environment), "%s", name);
        return 1;
    }
    if (strcmp(name, "quote") == 0 || strcmp(name, "quotation") == 0) {
        ++parser->quote_depth;
        return latex_literal(parser, "\n");
    }
    if (strcmp(name, "itemize") == 0 || strcmp(name, "enumerate") == 0) {
        if (parser->list_depth < LATEX_MAX_LIST_DEPTH) {
            parser->list_ordered[parser->list_depth] =
                strcmp(name, "enumerate") == 0;
            ++parser->list_depth;
        } else if (parser->list_overflow_depth < UINT_MAX) {
            ++parser->list_overflow_depth;
        }
        return latex_literal(parser, "\n");
    }
    return 1;
}

static int latex_end_environment(LatexParser *parser, const char *name)
{
    if (parser->in_code_environment && strcmp(parser->environment, name) == 0) {
        size_t i;
        size_t run = 0u;
        size_t max_run = 0u;
        size_t fence;
        for (i = 0u; i < parser->code.size; ++i) {
            if (parser->code.data[i] == '`') {
                if (++run > max_run) max_run = run;
            } else run = 0u;
        }
        fence = max_run < 2u ? 3u : max_run + 1u;
        for (i = 0u; i < fence; ++i)
            if (!latex_literal(parser, "`")) return 0;
        if (!latex_literal(parser, "\n") ||
            (parser->code.size && !latex_append(parser, parser->code.data,
                                                parser->code.size))) return 0;
        if (parser->code.size && parser->code.data[parser->code.size - 1u] != '\n' &&
            !latex_literal(parser, "\n")) return 0;
        for (i = 0u; i < fence; ++i)
            if (!latex_literal(parser, "`")) return 0;
        if (!latex_literal(parser, "\n\n")) return 0;
        free(parser->code.data);
        memset(&parser->code, 0, sizeof(parser->code));
        parser->in_code_environment = 0;
        parser->environment[0] = '\0';
        return 1;
    }
    if (parser->skip_environment && strcmp(parser->environment, name) == 0) {
        parser->skip_environment = 0;
        parser->environment[0] = '\0';
        return 1;
    }
    if (strcmp(name, "quote") == 0 || strcmp(name, "quotation") == 0) {
        if (parser->quote_depth > 0) --parser->quote_depth;
        return latex_literal(parser, "\n");
    }
    if (strcmp(name, "itemize") == 0 || strcmp(name, "enumerate") == 0) {
        if (parser->list_overflow_depth) --parser->list_overflow_depth;
        else if (parser->list_depth > 0) --parser->list_depth;
        return latex_literal(parser, "\n");
    }
    return 1;
}

static int latex_emit_item(LatexParser *parser, const uint8_t *text, size_t size)
{
    size_t i = 5u;
    size_t label_size;
    size_t depth;
    if (size < 5u || memcmp(text, "\\item", 5u) != 0) return 0;
    if (i < size && text[i] == '[') {
        size_t end = latex_matching(text, size, i, '[', ']');
        if (end != SIZE_MAX) i = end + 1u;
    }
    while (i < size && (text[i] == ' ' || text[i] == '\t')) ++i;
    depth = parser->list_depth ? parser->list_depth - 1u : 0u;
    if (depth > 8u) depth = 8u;
    for (size_t j = 0u; j < depth * 2u; ++j)
        if (!latex_literal(parser, " ")) return -1;
    if (parser->list_depth && parser->list_ordered[parser->list_depth - 1u]) {
        if (!latex_literal(parser, "1. ")) return -1;
    } else if (!latex_literal(parser, "- ")) return -1;
    label_size = size - i;
    if (!latex_inline(parser, text + i, label_size, 0u) ||
        !latex_literal(parser, "\n")) return -1;
    return 1;
}

static int latex_process_line(LatexParser *parser,
                              const uint8_t *text, size_t size)
{
    size_t left = 0u;
    size_t right = size;
    size_t after;
    size_t title_start;
    size_t title_size;
    size_t env_after;
    unsigned heading;
    int is_begin;
    char env[LATEX_MAX_ENV_NAME];
    if (parser->in_code_environment) {
        char closing[LATEX_MAX_ENV_NAME + 8u];
        size_t raw_start = 0u;
        size_t raw_end = size;
        while (raw_start < raw_end && (text[raw_start] == ' ' ||
                                       text[raw_start] == '\t')) ++raw_start;
        while (raw_end > raw_start && (text[raw_end - 1u] == ' ' ||
                                       text[raw_end - 1u] == '\t')) --raw_end;
        (void)snprintf(closing, sizeof(closing), "\\end{%s}", parser->environment);
        if (latex_equal(text + raw_start, raw_end - raw_start, closing))
            return latex_end_environment(parser, parser->environment);
        if (parser->code.size > LATEX_MAX_MARKDOWN_BYTES ||
            size >= LATEX_MAX_MARKDOWN_BYTES - parser->code.size ||
            !qw_text_buffer_append(&parser->code, (const char *)text, size) ||
            !qw_text_buffer_append(&parser->code, "\n", 1u)) {
            parser->error = parser->code.size > LATEX_MAX_MARKDOWN_BYTES ||
                size >= LATEX_MAX_MARKDOWN_BYTES - parser->code.size
                ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
            return 0;
        }
        return 1;
    }
    while (left < right && (text[left] == ' ' || text[left] == '\t')) ++left;
    while (right > left && (text[right - 1u] == ' ' || text[right - 1u] == '\t'))
        --right;
    if (left == right) return latex_literal(parser, "\n");
    text += left;
    size = right - left;

    if (parser->skip_environment) {
        char closing[LATEX_MAX_ENV_NAME + 8u];
        (void)snprintf(closing, sizeof(closing), "\\end{%s}", parser->environment);
        if (latex_equal(text, size, closing))
            return latex_end_environment(parser, parser->environment);
        return 1;
    }
    if (latex_environment_command(text, size, &is_begin, env,
                                  sizeof(env), &env_after)) {
        if (is_begin) return latex_begin_environment(parser, env);
        return latex_end_environment(parser, env);
    }
    if (latex_prefix(text, size, "\\end{document}")) return 2;
    if (latex_prefix(text, size, "\\maketitle")) {
        if (!parser->title_emitted && parser->title.size) {
            if (!latex_literal(parser, "# ") ||
                !latex_inline(parser, (const uint8_t *)parser->title.data,
                              parser->title.size, 0u) ||
                !latex_literal(parser, "\n\n")) return 0;
            parser->title_emitted = 1;
        }
        return 1;
    }
    if (latex_section(text, size, &heading, &title_start,
                      &title_size, &after)) {
        for (size_t i = 0u; i < heading; ++i)
            if (!latex_literal(parser, "#")) return 0;
        if (!latex_literal(parser, " ") ||
            !latex_inline(parser, text + title_start, title_size, 0u) ||
            !latex_literal(parser, "\n\n")) return 0;
        if (after < size && !latex_inline(parser, text + after,
                                          size - after, 0u)) return 0;
        return 1;
    }
    if (parser->list_depth && latex_prefix(text, size, "\\item"))
        return latex_emit_item(parser, text, size) >= 0;
    if (parser->quote_depth && !latex_literal(parser, "> ")) return 0;
    if (parser->list_depth) {
        size_t indent = parser->list_depth * 2u;
        if (indent > 16u) indent = 16u;
        for (size_t i = 0u; i < indent; ++i)
            if (!latex_literal(parser, " ")) return 0;
    }
    if (!latex_inline(parser, text, size, 0u)) return 0;
    return latex_literal(parser, "\n");
}

static int latex_translate(const uint8_t *data, size_t size,
                           LatexParser *parser)
{
    size_t position = 0u;
    size_t doc_start;
    size_t title_cmd;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    parser->has_document_environment =
        latex_find_command(data, size, "\\begin{document}", position) != SIZE_MAX;
    parser->in_document = !parser->has_document_environment;
    if (parser->has_document_environment) {
        title_cmd = latex_find_command(data, size, "\\title{", position);
        doc_start = latex_find_command(data, size, "\\begin{document}", position);
        if (title_cmd != SIZE_MAX && title_cmd < doc_start) {
            size_t start = title_cmd + 7u;
            size_t end = latex_matching(data, size, start - 1u, '{', '}');
            if (end != SIZE_MAX && end > start) {
                size_t title_size = end - start;
                if (title_size <= 65536u &&
                    !qw_text_buffer_append(&parser->title,
                        (const char *)data + start, title_size)) {
                    parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
                    return 0;
                }
            }
        }
    }
    while (position < size) {
        LatexLine line;
        size_t line_start;
        size_t body_start;
        if (!latex_line_read(data, size, &position, &line)) break;
        line_start = 0u;
        while (line_start < line.size &&
               (line.data[line_start] == ' ' || line.data[line_start] == '\t'))
            ++line_start;
        if (parser->has_document_environment && !parser->in_document) {
            body_start = latex_find(line.data, line.size,
                                    "\\begin{document}", line_start);
            if (body_start == SIZE_MAX) continue;
            parser->in_document = 1;
            body_start += strlen("\\begin{document}");
            if (body_start >= line.size) continue;
            if (latex_process_line(parser, line.data + body_start,
                                   line.size - body_start) == 0) return 0;
            continue;
        }
        if (parser->has_document_environment &&
            !parser->in_code_environment && !parser->skip_environment) {
            body_start = latex_find_command(line.data, line.size,
                                            "\\end{document}", line_start);
            if (body_start != SIZE_MAX) {
                int line_rc = latex_process_line(parser, line.data + line_start,
                                                 body_start - line_start);
                if (line_rc == 0) return 0;
                break;
            }
        }
        {
            const uint8_t *line_text = parser->in_code_environment
                ? line.data : line.data + line_start;
            size_t line_size = parser->in_code_environment
                ? line.size : line.size - line_start;
            int line_rc = latex_process_line(parser, line_text, line_size);
            if (line_rc == 0) return 0;
            if (line_rc == 2) break;
        }
    }
    if (parser->in_code_environment) {
        if (!latex_end_environment(parser, parser->environment)) return 0;
    }
    return 1;
}

int qw_latex_abi_version(void)
{
    return QW_LATEX_ABI_VERSION;
}

QwLatexStatus qw_latex_parse_markdown(const void *data, size_t size,
                                     QwLatexDocument *out_document)
{
    LatexParser parser;
    if (!data || !size || !out_document) return QW_LATEX_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (size > LATEX_MAX_INPUT_BYTES) return QW_LATEX_ERR_RESOURCE_LIMIT;
    if (!latex_utf8_valid((const uint8_t *)data, size))
        return QW_LATEX_ERR_INVALID_UTF8;
    memset(&parser, 0, sizeof(parser));
    parser.markdown.limit = LATEX_MAX_MARKDOWN_BYTES + 1u;
    parser.title.limit = 65537u;
    if (!latex_translate((const uint8_t *)data, size, &parser) ||
        (!parser.markdown.size && !latex_literal(&parser, "\n"))) {
        QwLatexStatus status = parser.error == QW_STATUS_ERR_RESOURCE_LIMIT
            ? QW_LATEX_ERR_RESOURCE_LIMIT : QW_LATEX_ERR_OUT_OF_MEMORY;
        free(parser.markdown.data);
        free(parser.code.data);
        free(parser.title.data);
        return status;
    }
    free(parser.code.data);
    out_document->markdown = parser.markdown.data;
    out_document->markdown_size = parser.markdown.size;
    out_document->title = parser.title.data;
    out_document->title_size = parser.title.size;
    out_document->skipped_commands = parser.skipped_commands;
    out_document->omitted_images = parser.omitted_images;
    out_document->omitted_tables = parser.omitted_tables;
    out_document->omitted_figures = parser.omitted_figures;
    out_document->reduced_math = parser.reduced_math;
    out_document->dropped_unsafe_links = parser.skipped_unsafe_links;
    return QW_LATEX_OK;
}

void qw_latex_document_free(QwLatexDocument *document)
{
    if (!document) return;
    free(document->markdown);
    free(document->title);
    memset(document, 0, sizeof(*document));
}

const char *qw_latex_status_string(QwLatexStatus status)
{
    switch (status) {
    case QW_LATEX_OK: return "success";
    case QW_LATEX_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_LATEX_ERR_INVALID_UTF8: return "input is not valid UTF-8";
    case QW_LATEX_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_LATEX_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    default: return "unknown LaTeX parser status";
    }
}

#ifndef QUIREWISE_STANDALONE_LATEX_PARSER

static int latex_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t position = 0u;
    size_t scan_size;
    if (!input || !size || size > LATEX_MAX_INPUT_BYTES) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    scan_size = size < LATEX_PROBE_BYTES ? size : LATEX_PROBE_BYTES;
    if (!latex_utf8_valid(data + position, scan_size - position)) return 0;
    if (latex_find(data + position, scan_size - position,
                  "\\documentclass", 0u) != SIZE_MAX ||
        latex_find(data + position, scan_size - position,
                  "\\begin{document}", 0u) != SIZE_MAX ||
        latex_find(data + position, scan_size - position,
                  "\\section{", 0u) != SIZE_MAX ||
        latex_find(data + position, scan_size - position,
                  "\\chapter{", 0u) != SIZE_MAX ||
        latex_find(data + position, scan_size - position,
                  "\\begin{itemize}", 0u) != SIZE_MAX) return 1;
    return 0;
}

static int latex_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    return dot && (latex_equal((const uint8_t *)dot, strlen(dot), ".tex") ||
                   latex_equal((const uint8_t *)dot, strlen(dot), ".latex"));
}

static int latex_open_layout(const void *input, size_t input_size,
                             QwInputFormat input_format,
                             const QwConvertOptions *options,
                             QwLayoutDocument *out_document,
                             QwConvertReport *report)
{
    QwLatexDocument latex = {0};
    int rc;
    (void)options;
    if (!input || !input_size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_TEX) return QW_STATUS_ERR_INVALID_ARGUMENT;
    {
        QwLatexStatus status = qw_latex_parse_markdown(input, input_size,
                                                       &latex);
        if (status == QW_LATEX_ERR_INVALID_ARGUMENT)
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        if (status == QW_LATEX_ERR_INVALID_UTF8) return QW_STATUS_ERR_DECODE;
        if (status == QW_LATEX_ERR_RESOURCE_LIMIT) return QW_STATUS_ERR_RESOURCE_LIMIT;
        if (status != QW_LATEX_OK) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    rc = qw_markdown_format_open_layout(latex.markdown,
                                        latex.markdown_size,
                                        QW_FORMAT_MARKDOWN, NULL,
                                        out_document, report);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        (void)snprintf(report->note, sizeof(report->note),
            "LaTeX UTF-8 subset rendered through CommonMark; macros are not expanded%s%s%s%s%s",
            latex.omitted_images ? "; images were replaced by text" : "",
            latex.omitted_tables ? "; tables were omitted" : "",
            latex.omitted_figures ? "; figures were omitted" : "",
            latex.reduced_math ? "; math was reduced to plain text" : "",
            latex.dropped_unsafe_links ? "; unsafe links were reduced to label text" : "");
    }
    qw_latex_document_free(&latex);
    return rc;
}

const QwFormatDescriptor qw_format_tex = {
    .id = QW_FORMAT_TEX,
    .name = "LaTeX",
    .probe = latex_probe,
    .open_layout = latex_open_layout,
    .max_input_bytes = LATEX_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};

const QwPathFormatDescriptor qw_path_format_tex = {
    .id = QW_FORMAT_TEX,
    .probe_path = latex_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};

#endif

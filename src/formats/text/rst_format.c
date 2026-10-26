#ifndef QUIREWISE_STANDALONE_RST_PARSER
#include "../../core/format_registry.h"
#include "markdown_format.h"
#endif
#include "../../core/text_buffer.h"

#include <quirewise/rst.h>
#include "quirewise/status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RST_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    RST_MAX_MARKDOWN_BYTES = 128u * 1024u * 1024u,
    RST_PROBE_BYTES = 65536u
};

typedef struct RstLine {
    const uint8_t *data;
    size_t size;
} RstLine;

typedef struct RstParser {
    QwTextBuffer markdown;
    int error;
    int ignored_directive;
    int omitted_image;
    int dropped_link;
} RstParser;

static int rst_append(RstParser *parser, const char *text, size_t size)
{
    if (parser->markdown.size > RST_MAX_MARKDOWN_BYTES ||
        size > RST_MAX_MARKDOWN_BYTES - parser->markdown.size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&parser->markdown, text, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int rst_literal(RstParser *parser, const char *text)
{
    return rst_append(parser, text, strlen(text));
}

static int rst_append_repeated(RstParser *parser, char value, size_t count)
{
    char chunk[32];
    memset(chunk, value, sizeof(chunk));
    while (count) {
        size_t amount = count < sizeof(chunk) ? count : sizeof(chunk);
        if (!rst_append(parser, chunk, amount)) return 0;
        count -= amount;
    }
    return 1;
}

static int rst_read_line(const uint8_t *data, size_t size, size_t *position,
                         RstLine *out_line)
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

static size_t rst_indent(const RstLine *line, size_t *out_bytes)
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

static int rst_line_blank(const RstLine *line)
{
    size_t i;
    for (i = 0u; i < line->size; ++i) {
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    }
    return 1;
}

static size_t rst_trimmed_size(const RstLine *line)
{
    size_t size = line->size;
    while (size && (line->data[size - 1u] == ' ' ||
                    line->data[size - 1u] == '\t')) --size;
    return size;
}

static int rst_adornment_char(uint8_t value)
{
    return value == '=' || value == '-' || value == '~' || value == '^' ||
           value == '"' || value == '\'' || value == '`' || value == ':' ||
           value == '#' || value == '*' || value == '+' || value == '_';
}

static int rst_is_adornment(const RstLine *line, uint8_t *out_char)
{
    size_t size = rst_trimmed_size(line);
    size_t i;
    uint8_t value;
    if (size < 4u || !rst_adornment_char(line->data[0])) return 0;
    value = line->data[0];
    for (i = 1u; i < size; ++i) {
        if (line->data[i] != value) return 0;
    }
    if (out_char) *out_char = value;
    return 1;
}

static unsigned rst_heading_level(uint8_t value)
{
    switch (value) {
    case '=': return 1u;
    case '-': return 2u;
    case '~': return 3u;
    case '^': return 4u;
    case '"': return 5u;
    default: return 6u;
    }
}

static int rst_ascii_equal(const uint8_t *text, size_t size,
                           const char *expected)
{
    size_t i;
    size_t expected_size = strlen(expected);
    if (size != expected_size) return 0;
    for (i = 0u; i < size; ++i) {
        uint8_t actual = text[i];
        if (actual >= 'A' && actual <= 'Z') actual += (uint8_t)('a' - 'A');
        if (actual != (uint8_t)expected[i]) return 0;
    }
    return 1;
}

static int rst_ascii_prefix(const uint8_t *text, size_t size,
                            const char *prefix)
{
    size_t prefix_size = strlen(prefix);
    size_t i;
    if (size < prefix_size) return 0;
    for (i = 0u; i < prefix_size; ++i) {
        uint8_t actual = text[i];
        uint8_t expected = (uint8_t)prefix[i];
        if (actual >= 'A' && actual <= 'Z') actual += (uint8_t)('a' - 'A');
        if (actual != expected) return 0;
    }
    return 1;
}

static int rst_url_safe(const uint8_t *url, size_t size)
{
    size_t i;
    if (!(rst_ascii_prefix(url, size, "http://") ||
          rst_ascii_prefix(url, size, "https://") ||
          rst_ascii_prefix(url, size, "mailto:"))) return 0;
    for (i = 0u; i < size; ++i) {
        if (url[i] <= 0x20u || url[i] == 0x7fu || url[i] == '<' ||
            url[i] == '>' || url[i] == '"') return 0;
    }
    return 1;
}

static int rst_append_link_label(RstParser *parser,
                                 const uint8_t *text, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        if (text[i] == '\\' || text[i] == '[' || text[i] == ']') {
            char escaped[2] = {'\\', (char)text[i]};
            if (!rst_append(parser, escaped, sizeof(escaped))) return 0;
        } else if (!rst_append(parser, (const char *)text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

static size_t rst_max_backtick_run(const uint8_t *text, size_t size)
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

static int rst_append_code_span(RstParser *parser,
                               const uint8_t *text, size_t size)
{
    size_t fence = rst_max_backtick_run(text, size) + 1u;
    if (!fence) fence = 1u;
    if (!rst_append_repeated(parser, '`', fence)) return 0;
    if (size && (text[0] == ' ' || text[0] == '`') &&
        (text[size - 1u] == ' ' || text[size - 1u] == '`')) {
        if (!rst_literal(parser, " ") ||
            !rst_append(parser, (const char *)text, size) ||
            !rst_literal(parser, " ")) return 0;
    } else if (!rst_append(parser, (const char *)text, size)) {
        return 0;
    }
    return rst_append_repeated(parser, '`', fence);
}

static int rst_inline(RstParser *parser, const uint8_t *text, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        if (i + 1u < size && text[i] == '`' && text[i + 1u] == '`') {
            size_t end = i + 2u;
            while (end + 1u < size &&
                   !(text[end] == '`' && text[end + 1u] == '`')) ++end;
            if (end + 1u < size) {
                if (!rst_append_code_span(parser, text + i + 2u,
                                          end - i - 2u)) return 0;
                i = end + 2u;
                continue;
            }
        }
        if (text[i] == '`') {
            size_t end = i + 1u;
            size_t separator = SIZE_MAX;
            while (end < size && text[end] != '`') {
                if (text[end] == '<' && end > i + 1u && text[end - 1u] == ' ')
                    separator = end - 1u;
                ++end;
            }
            if (end + 1u < size && text[end] == '`' && text[end + 1u] == '_' &&
                separator != SIZE_MAX && end > separator + 2u &&
                text[end - 1u] == '>') {
                size_t label_size = separator - i - 1u;
                const uint8_t *url = text + separator + 2u;
                size_t url_size = end - separator - 3u;
                if (rst_url_safe(url, url_size)) {
                    if (!rst_literal(parser, "[") ||
                        !rst_append_link_label(parser, text + i + 1u,
                                               label_size) ||
                        !rst_literal(parser, "](<")) return 0;
                    if (!rst_append(parser, (const char *)url, url_size) ||
                        !rst_literal(parser, ">)")) return 0;
                } else {
                    parser->dropped_link = 1;
                    if (!rst_append_link_label(parser, text + i + 1u,
                                               label_size)) return 0;
                }
                i = end + 2u;
                continue;
            }
        }
        if (!rst_append(parser, (const char *)text + i, 1u)) return 0;
        ++i;
    }
    return 1;
}

static int rst_directive(const RstLine *line, const char *name,
                         size_t *out_argument, size_t *out_argument_size)
{
    size_t indent_bytes;
    size_t indent = rst_indent(line, &indent_bytes);
    size_t name_size = strlen(name);
    size_t position = indent_bytes;
    (void)indent;
    if (line->size - position < 3u || line->data[position] != '.' ||
        line->data[position + 1u] != '.' || line->data[position + 2u] != ' ')
        return 0;
    position += 3u;
    if (line->size - position < name_size ||
        !rst_ascii_equal(line->data + position, name_size, name)) return 0;
    position += name_size;
    if (line->size - position < 2u || line->data[position] != ':' ||
        line->data[position + 1u] != ':') return 0;
    position += 2u;
    while (position < line->size &&
           (line->data[position] == ' ' || line->data[position] == '\t')) ++position;
    if (out_argument) *out_argument = position;
    if (out_argument_size) *out_argument_size = line->size - position;
    return 1;
}

static size_t rst_strip_indent(const RstLine *line, size_t columns)
{
    size_t bytes = 0u;
    size_t removed = 0u;
    while (bytes < line->size && removed < columns &&
           (line->data[bytes] == ' ' || line->data[bytes] == '\t')) {
        size_t next = line->data[bytes] == '\t'
            ? (removed + 8u) & ~(size_t)7u : removed + 1u;
        if (next > columns) break;
        removed = next;
        ++bytes;
    }
    return bytes;
}

static size_t rst_max_fence_run(const uint8_t *text, size_t size, uint8_t value)
{
    size_t maximum = 0u;
    size_t current = 0u;
    size_t i;
    for (i = 0u; i < size; ++i) {
        if (text[i] == value) {
            ++current;
            if (current > maximum) maximum = current;
        } else {
            current = 0u;
        }
    }
    return maximum;
}

static int rst_collect_code(RstParser *parser, const uint8_t *data,
                            size_t size, size_t *position,
                            size_t minimum_indent)
{
    QwTextBuffer code = {0};
    size_t base_indent = 0u;
    int started = 0;
    int rc = 1;
    code.limit = RST_MAX_MARKDOWN_BYTES + 1u;
    while (*position < size) {
        size_t before = *position;
        RstLine line;
        size_t indent;
        if (!rst_read_line(data, size, position, &line)) break;
        indent = rst_indent(&line, NULL);
        if (rst_line_blank(&line)) {
            size_t scan = before;
            size_t blank_lines = 0u;
            RstLine lookahead;
            while (rst_read_line(data, size, &scan, &lookahead) &&
                   rst_line_blank(&lookahead)) ++blank_lines;
            if (!rst_read_line(data, size, &scan, &lookahead) ||
                rst_line_blank(&lookahead) ||
                rst_indent(&lookahead, NULL) <= minimum_indent ||
                (started && rst_indent(&lookahead, NULL) < base_indent)) {
                *position = before;
                break;
            }
            if (started) {
                size_t i;
                for (i = 0u; i < blank_lines; ++i) {
                    if (!qw_text_buffer_append(&code, "\n", 1u)) {
                        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
                        rc = 0;
                        goto cleanup;
                    }
                }
            }
            *position = scan - lookahead.size;
            /* Restore the position to the beginning of the nonblank lookahead line. */
            while (*position && data[*position - 1u] != '\n' &&
                   data[*position - 1u] != '\r') --*position;
            continue;
        }
        if (indent <= minimum_indent) {
            *position = before;
            break;
        }
        if (!started) {
            base_indent = indent;
            started = 1;
        }
        if (indent < base_indent) {
            *position = before;
            break;
        }
        {
            size_t strip = rst_strip_indent(&line, base_indent);
            if (line.size > strip &&
                !qw_text_buffer_append(&code, (const char *)line.data + strip,
                                       line.size - strip)) {
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
    }
    {
        size_t ticks = rst_max_fence_run((const uint8_t *)code.data,
                                         code.size, '`');
        size_t tildes = rst_max_fence_run((const uint8_t *)code.data,
                                          code.size, '~');
        uint8_t fence_char = ticks <= tildes ? '`' : '~';
        size_t fence_size = (ticks <= tildes ? ticks : tildes) + 1u;
        if (fence_size < 3u) fence_size = 3u;
        if (!rst_literal(parser, "\n") ||
            !rst_append_repeated(parser, (char)fence_char, fence_size) ||
            !rst_literal(parser, "\n") ||
            (code.size && !rst_append(parser, code.data, code.size)) ||
            (code.size && code.data[code.size - 1u] != '\n' &&
             !rst_literal(parser, "\n")) ||
            !rst_append_repeated(parser, (char)fence_char, fence_size) ||
            !rst_literal(parser, "\n\n")) rc = 0;
    }
    if (!rc && !parser->error) parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;

cleanup:
    free(code.data);
    return rc;
}

static int rst_emit_admonition(RstParser *parser, const uint8_t *data,
                               size_t size, size_t *position,
                               size_t minimum_indent, const char *label)
{
    if (!rst_literal(parser, "\n> **") || !rst_literal(parser, label) ||
        !rst_literal(parser, ":**\n")) return 0;
    while (*position < size) {
        size_t before = *position;
        RstLine line;
        size_t indent_bytes;
        size_t indent;
        if (!rst_read_line(data, size, position, &line)) break;
        indent = rst_indent(&line, &indent_bytes);
        if (rst_line_blank(&line)) {
            size_t scan = *position;
            RstLine next;
            if (!rst_read_line(data, size, &scan, &next) ||
                rst_line_blank(&next) || rst_indent(&next, NULL) <= minimum_indent) {
                *position = before;
                break;
            }
            if (!rst_literal(parser, ">\n")) return 0;
            continue;
        }
        if (indent <= minimum_indent) {
            *position = before;
            break;
        }
        if (!rst_literal(parser, "> ") ||
            !rst_inline(parser, line.data + indent_bytes,
                        line.size - indent_bytes) ||
            !rst_literal(parser, "\n")) return 0;
    }
    return rst_literal(parser, "\n");
}

static int rst_skip_directive_body(const uint8_t *data, size_t size,
                                   size_t *position, size_t minimum_indent)
{
    while (*position < size) {
        size_t before = *position;
        RstLine line;
        if (!rst_read_line(data, size, position, &line)) break;
        if (rst_line_blank(&line) || rst_indent(&line, NULL) > minimum_indent)
            continue;
        *position = before;
        break;
    }
    return 1;
}

static int rst_emit_heading(RstParser *parser, const RstLine *title,
                            uint8_t adornment)
{
    size_t indent_bytes;
    (void)rst_indent(title, &indent_bytes);
    size_t level = rst_heading_level(adornment);
    if (!rst_literal(parser, "\n") ||
        !rst_append_repeated(parser, '#', level) ||
        !rst_literal(parser, " ") ||
        !rst_inline(parser, title->data + indent_bytes,
                    rst_trimmed_size(title) - indent_bytes) ||
        !rst_literal(parser, "\n\n")) return 0;
    return 1;
}

static int rst_has_indented_following(const uint8_t *data, size_t size,
                                     size_t position, size_t minimum_indent)
{
    while (position < size) {
        RstLine line;
        if (!rst_read_line(data, size, &position, &line)) break;
        if (rst_line_blank(&line)) continue;
        return rst_indent(&line, NULL) > minimum_indent;
    }
    return 0;
}

static int rst_translate(const uint8_t *data, size_t size, RstParser *parser)
{
    size_t position = 0u;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    while (position < size) {
        RstLine line;
        size_t directive_position;
        size_t argument;
        size_t argument_size;
        size_t indent_bytes;
        size_t indent;
        size_t line_size;
        uint8_t adornment;
        if (!rst_read_line(data, size, &position, &line)) break;
        indent = rst_indent(&line, &indent_bytes);
        line_size = rst_trimmed_size(&line);
        if (rst_line_blank(&line)) {
            if (!rst_literal(parser, "\n")) return 0;
            continue;
        }

        if (rst_directive(&line, "code-block", &argument, &argument_size) ||
            rst_directive(&line, "code", &argument, &argument_size)) {
            directive_position = position;
            while (directive_position < size) {
                size_t before = directive_position;
                RstLine option;
                size_t option_indent;
                size_t option_start;
                if (!rst_read_line(data, size, &directive_position, &option)) break;
                option_indent = rst_indent(&option, &option_start);
                if (rst_line_blank(&option) ||
                    (option_indent > indent && option_start < option.size &&
                     option.data[option_start] == ':')) continue;
                directive_position = before;
                break;
            }
            position = directive_position;
            if (!rst_collect_code(parser, data, size, &position, indent)) return 0;
            continue;
        }
        if (rst_directive(&line, "image", &argument, &argument_size)) {
            if (!rst_literal(parser, "Image omitted: ") ||
                !rst_inline(parser, line.data + argument, argument_size) ||
                !rst_literal(parser, "\n\n")) return 0;
            parser->omitted_image = 1;
            rst_skip_directive_body(data, size, &position, indent);
            continue;
        }
        {
            static const struct { const char *name; const char *label; } admonitions[] = {
                {"note", "Note"}, {"attention", "Attention"},
                {"caution", "Caution"}, {"danger", "Danger"},
                {"error", "Error"}, {"hint", "Hint"},
                {"important", "Important"}, {"tip", "Tip"},
                {"warning", "Warning"}
            };
            size_t i;
            for (i = 0u; i < sizeof(admonitions) / sizeof(admonitions[0]); ++i) {
                if (rst_directive(&line, admonitions[i].name, NULL, NULL)) {
                    if (!rst_emit_admonition(parser, data, size, &position,
                                             indent, admonitions[i].label)) return 0;
                    break;
                }
            }
            if (i < sizeof(admonitions) / sizeof(admonitions[0])) continue;
        }
        if (line_size >= 3u && line.data[indent_bytes] == '.' &&
            line.data[indent_bytes + 1u] == '.' &&
            (line.data[indent_bytes + 2u] == ' ' ||
             line.data[indent_bytes + 2u] == '\t')) {
            parser->ignored_directive = 1;
            rst_skip_directive_body(data, size, &position, indent);
            continue;
        }

        if (rst_is_adornment(&line, &adornment)) {
            size_t look = position;
            RstLine title;
            RstLine underline;
            uint8_t below_adornment;
            if (rst_read_line(data, size, &look, &title) &&
                !rst_line_blank(&title) &&
                rst_read_line(data, size, &look, &underline) &&
                rst_is_adornment(&underline, &below_adornment) &&
                below_adornment == adornment &&
                rst_trimmed_size(&underline) >= rst_trimmed_size(&title)) {
                if (!rst_emit_heading(parser, &title, adornment)) return 0;
                position = look;
                continue;
            }
            if (!rst_literal(parser, "\n---\n\n")) return 0;
            continue;
        }
        {
            size_t look = position;
            RstLine underline;
            uint8_t underline_char;
            if (rst_read_line(data, size, &look, &underline) &&
                rst_is_adornment(&underline, &underline_char) &&
                rst_trimmed_size(&underline) >= line_size) {
                if (!rst_emit_heading(parser, &line, underline_char)) return 0;
                position = look;
                continue;
            }
        }
        if (line_size >= 2u && line.data[line_size - 1u] == ':' &&
            line.data[line_size - 2u] == ':' &&
            rst_has_indented_following(data, size, position, indent)) {
            if (!rst_inline(parser, line.data + indent_bytes,
                            line_size - indent_bytes - 1u) ||
                !rst_literal(parser, "\n\n") ||
                !rst_collect_code(parser, data, size, &position, indent)) return 0;
            continue;
        }
        if (!rst_inline(parser, line.data, line_size) ||
            !rst_literal(parser, "\n")) return 0;
    }
    if (!parser->markdown.size && !rst_literal(parser, "\n")) return 0;
    return 1;
}

int qw_rst_abi_version(void)
{
    return QW_RST_ABI_VERSION;
}

QwRstStatus qw_rst_parse_markdown(const void *data, size_t size,
                                 QwRstDocument *out_document)
{
    RstParser parser;
    if (!data || !size || !out_document) return QW_RST_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (size > RST_MAX_INPUT_BYTES) return QW_RST_ERR_RESOURCE_LIMIT;
    memset(&parser, 0, sizeof(parser));
    parser.markdown.limit = RST_MAX_MARKDOWN_BYTES + 1u;
    if (!rst_translate((const uint8_t *)data, size, &parser)) {
        QwRstStatus status = parser.error == QW_STATUS_ERR_RESOURCE_LIMIT
            ? QW_RST_ERR_RESOURCE_LIMIT : QW_RST_ERR_OUT_OF_MEMORY;
        free(parser.markdown.data);
        return status;
    }
    out_document->markdown = parser.markdown.data;
    out_document->markdown_size = parser.markdown.size;
    out_document->ignored_directives = parser.ignored_directive;
    out_document->omitted_images = parser.omitted_image;
    out_document->dropped_unsafe_links = parser.dropped_link;
    return QW_RST_OK;
}

void qw_rst_document_free(QwRstDocument *document)
{
    if (!document) return;
    free(document->markdown);
    memset(document, 0, sizeof(*document));
}

const char *qw_rst_status_string(QwRstStatus status)
{
    switch (status) {
    case QW_RST_OK: return "success";
    case QW_RST_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_RST_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_RST_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    default: return "unknown reStructuredText parser status";
    }
}

#ifndef QUIREWISE_STANDALONE_RST_PARSER

static int rst_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t scan_size;
    size_t position = 0u;
    if (!input || !size || size > RST_MAX_INPUT_BYTES) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    scan_size = size < RST_PROBE_BYTES ? size : RST_PROBE_BYTES;
    while (position < scan_size) {
        RstLine line;
        size_t next;
        RstLine following;
        uint8_t adornment;
        if (!rst_read_line(data, scan_size, &position, &line)) break;
        if (rst_directive(&line, "code-block", NULL, NULL) ||
            rst_directive(&line, "code", NULL, NULL) ||
            rst_directive(&line, "note", NULL, NULL) ||
            rst_directive(&line, "attention", NULL, NULL) ||
            rst_directive(&line, "caution", NULL, NULL) ||
            rst_directive(&line, "danger", NULL, NULL) ||
            rst_directive(&line, "error", NULL, NULL) ||
            rst_directive(&line, "hint", NULL, NULL) ||
            rst_directive(&line, "important", NULL, NULL) ||
            rst_directive(&line, "tip", NULL, NULL) ||
            rst_directive(&line, "warning", NULL, NULL) ||
            rst_directive(&line, "image", NULL, NULL)) return 1;
        next = position;
        if (rst_read_line(data, scan_size, &next, &following) &&
            rst_is_adornment(&following, &adornment) &&
            rst_trimmed_size(&following) >= rst_trimmed_size(&line) &&
            !rst_line_blank(&line)) return 1;
        if (rst_is_adornment(&line, &adornment)) {
            size_t overline_position = position;
            RstLine title;
            RstLine underline;
            uint8_t underline_char;
            if (rst_read_line(data, scan_size, &overline_position, &title) &&
                !rst_line_blank(&title) &&
                rst_read_line(data, scan_size, &overline_position, &underline) &&
                rst_is_adornment(&underline, &underline_char) &&
                underline_char == adornment &&
                rst_trimmed_size(&underline) >= rst_trimmed_size(&title)) return 1;
        }
        if (line.size >= 2u && line.data[line.size - 1u] == ':' &&
            line.data[line.size - 2u] == ':' &&
            rst_has_indented_following(data, scan_size, position,
                                       rst_indent(&line, NULL))) return 1;
        if (memchr(line.data, '`', line.size) &&
            memchr(line.data, '<', line.size) &&
            memchr(line.data, '>', line.size)) return 1;
    }
    return 0;
}

static int rst_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    return dot && rst_ascii_equal((const uint8_t *)dot, strlen(dot), ".rst");
}

static int rst_open_layout(const void *input, size_t input_size,
                           QwInputFormat input_format,
                           const QwConvertOptions *options,
                           QwLayoutDocument *out_document,
                           QwConvertReport *report)
{
    QwRstDocument rst = {0};
    int ignored_directives;
    int omitted_images;
    int dropped_unsafe_links;
    int rc;
    (void)options;
    if (!input || !input_size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_RST) return QW_STATUS_ERR_INVALID_ARGUMENT;
    {
        QwRstStatus status = qw_rst_parse_markdown(input, input_size, &rst);
        if (status == QW_RST_ERR_INVALID_ARGUMENT)
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        if (status == QW_RST_ERR_RESOURCE_LIMIT)
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        if (status != QW_RST_OK) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    ignored_directives = rst.ignored_directives;
    omitted_images = rst.omitted_images;
    dropped_unsafe_links = rst.dropped_unsafe_links;
    rc = qw_markdown_format_open_layout(rst.markdown,
                                        rst.markdown_size,
                                        QW_FORMAT_MARKDOWN, NULL,
                                        out_document, report);
    qw_rst_document_free(&rst);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        (void)snprintf(report->note, sizeof(report->note),
            "reStructuredText subset rendered through CommonMark; unsupported directives are omitted%s%s%s",
            ignored_directives ? "; some directives were skipped" : "",
            omitted_images ? "; images were replaced by text" : "",
            dropped_unsafe_links ? "; unsafe links were reduced to label text" : "");
    }
    return rc;
}

const QwFormatDescriptor qw_format_rst = {
    .id = QW_FORMAT_RST,
    .name = "reStructuredText",
    .probe = rst_probe,
    .open_layout = rst_open_layout,
    .max_input_bytes = RST_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};

const QwPathFormatDescriptor qw_path_format_rst = {
    .id = QW_FORMAT_RST,
    .probe_path = rst_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};

#endif

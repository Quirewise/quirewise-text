#ifndef QUIREWISE_STANDALONE_MAN_PARSER
#include "../../core/format_registry.h"
#endif
#include "../../core/layout_document.h"
#include "../../core/text_buffer.h"
#include "quirewise/man.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAN_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    MAN_MAX_OUTPUT_BYTES = 128u * 1024u * 1024u,
    MAN_MAX_INDENT_DEPTH = 32u
};

typedef struct ManLine {
    const uint8_t *data;
    size_t size;
} ManLine;

typedef struct ManParser {
    QwTextBuffer html;
    size_t indent_depth;
    int paragraph_open;
    int paragraph_has_text;
    int expect_term;
    int term_definition;
    int in_preformatted;
    int in_macro_definition;
    int saw_title;
    int saw_section;
    int saw_content;
    int saw_ignored_macro;
    int error;
} ManParser;

static int man_utf8_valid(const uint8_t *data, size_t size)
{
    size_t position = 0u;
    if (!data && size) return 0;
    while (position < size) {
        uint8_t first = data[position++];
        size_t continuation_count;
        uint32_t codepoint;
        uint32_t minimum;
        size_t i;
        if (first < 0x80u) {
            if (first == 0u || (first < 0x20u && first != '\t' &&
                first != '\r' && first != '\n')) return 0;
            continue;
        }
        if ((first & 0xe0u) == 0xc0u) {
            continuation_count = 1u; codepoint = first & 0x1fu; minimum = 0x80u;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation_count = 2u; codepoint = first & 0x0fu; minimum = 0x800u;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation_count = 3u; codepoint = first & 0x07u; minimum = 0x10000u;
        } else {
            return 0;
        }
        if (continuation_count > size - position) return 0;
        for (i = 0u; i < continuation_count; ++i) {
            uint8_t next = data[position++];
            if ((next & 0xc0u) != 0x80u) return 0;
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return 0;
    }
    return 1;
}

static int man_append(ManParser *parser, const void *data, size_t size)
{
    if (!parser || (!data && size)) return 0;
    if (parser->html.size > MAN_MAX_OUTPUT_BYTES ||
        size > MAN_MAX_OUTPUT_BYTES - parser->html.size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&parser->html, (const char *)data, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int man_literal(ManParser *parser, const char *text)
{
    return man_append(parser, text, strlen(text));
}

static int man_html_escape(ManParser *parser, const uint8_t *data, size_t size)
{
    size_t i;
    size_t span = 0u;
    for (i = 0u; i < size; ++i) {
        const char *replacement = NULL;
        size_t replacement_size = 0u;
        switch (data[i]) {
        case '&': replacement = "&amp;"; replacement_size = 5u; break;
        case '<': replacement = "&lt;"; replacement_size = 4u; break;
        case '>': replacement = "&gt;"; replacement_size = 4u; break;
        case '"': replacement = "&quot;"; replacement_size = 6u; break;
        case '\'': replacement = "&#39;"; replacement_size = 5u; break;
        default: break;
        }
        if (!replacement) continue;
        if (i > span && !man_append(parser, data + span, i - span)) return 0;
        if (!man_append(parser, replacement, replacement_size)) return 0;
        span = i + 1u;
    }
    return span == size || man_append(parser, data + span, size - span);
}

static int man_html_text(ManParser *parser, const uint8_t *data, size_t size)
{
    int bold = 0;
    int italic = 0;
    size_t i = 0u;
    size_t span = 0u;
    while (i < size) {
        uint8_t value = data[i];
        if (value != '\\' || i + 1u >= size) {
            ++i;
            continue;
        }
        if (i > span && !man_html_escape(parser, data + span, i - span)) return 0;
        ++i;
        value = data[i++];
        if (value == 'f' && i < size) {
            uint8_t font = data[i++];
            int next_bold = font == 'B' || font == '3' || font == '6';
            int next_italic = font == 'I' || font == '2';
            if (bold && !next_bold && !man_literal(parser, "</strong>")) return 0;
            if (italic && !next_italic && !man_literal(parser, "</em>")) return 0;
            if (!bold && next_bold && !man_literal(parser, "<strong>")) return 0;
            if (!italic && next_italic && !man_literal(parser, "<em>")) return 0;
            bold = next_bold;
            italic = next_italic;
        } else if (value == 'e' || value == '\\') {
            if (!man_html_escape(parser, (const uint8_t *)"\\", 1u)) return 0;
        } else if (value == '-' ) {
            if (!man_html_escape(parser, (const uint8_t *)"-", 1u)) return 0;
        } else if (value == '&' || value == '%') {
            /* Zero-width and discretionary-hyphen escapes have no visible text. */
        } else if (value == '~' || value == ' ') {
            if (!man_literal(parser, "&nbsp;")) return 0;
        } else if (value == '(' && i + 1u < size) {
            char code[3] = {(char)data[i], (char)data[i + 1u], '\0'};
            const char *replacement = NULL;
            i += 2u;
            if (strcmp(code, "em") == 0) replacement = "—";
            else if (strcmp(code, "en") == 0) replacement = "–";
            else if (strcmp(code, "bu") == 0) replacement = "•";
            else if (strcmp(code, "co") == 0) replacement = "©";
            else if (strcmp(code, "rg") == 0) replacement = "®";
            else if (strcmp(code, "tm") == 0) replacement = "™";
            else if (strcmp(code, "lq") == 0) replacement = "“";
            else if (strcmp(code, "rq") == 0) replacement = "”";
            if (replacement && !man_literal(parser, replacement)) return 0;
        } else if (value == 'c') {
            /* Suppress only the visual line break; source lines stay bounded. */
        } else if (value == '"') {
            span = i;
            break;
        } else if (!man_html_escape(parser, &value, 1u)) {
            return 0;
        }
        span = i;
    }
    if (i > span && !man_html_escape(parser, data + span, i - span)) return 0;
    if (italic && !man_literal(parser, "</em>")) return 0;
    if (bold && !man_literal(parser, "</strong>")) return 0;
    return 1;
}

static int man_close_paragraph(ManParser *parser)
{
    if (!parser->paragraph_open) return 1;
    if (!man_literal(parser, "</p>")) return 0;
    parser->paragraph_open = 0;
    parser->paragraph_has_text = 0;
    parser->term_definition = 0;
    return 1;
}

static int man_open_paragraph(ManParser *parser)
{
    if (parser->paragraph_open || parser->in_preformatted) return 1;
    if (!man_literal(parser, "<p>")) return 0;
    parser->paragraph_open = 1;
    parser->paragraph_has_text = 0;
    return 1;
}

static int man_render_inline(ManParser *parser, const uint8_t *data, size_t size,
                             const char *wrapper)
{
    if (wrapper && !man_literal(parser, wrapper)) return 0;
    if (!man_html_text(parser, data, size)) return 0;
    if (wrapper && strcmp(wrapper, "<strong>") == 0)
        return man_literal(parser, "</strong>");
    if (wrapper && strcmp(wrapper, "<em>") == 0)
        return man_literal(parser, "</em>");
    if (wrapper && strcmp(wrapper, "<small>") == 0)
        return man_literal(parser, "</small>");
    if (wrapper && strcmp(wrapper, "<small><strong>") == 0)
        return man_literal(parser, "</strong></small>");
    return 1;
}

static int man_line_is_blank(const ManLine *line)
{
    size_t i;
    for (i = 0u; i < line->size; ++i) {
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    }
    return 1;
}

static int man_name_equal(const uint8_t *data, size_t size, const char *name)
{
    size_t i;
    size_t name_size = strlen(name);
    if (size != name_size) return 0;
    for (i = 0u; i < size; ++i) {
        uint8_t left = data[i];
        uint8_t right = (uint8_t)name[i];
        if (left >= 'a' && left <= 'z') left = (uint8_t)(left - ('a' - 'A'));
        if (right >= 'a' && right <= 'z') right = (uint8_t)(right - ('a' - 'A'));
        if (left != right) return 0;
    }
    return 1;
}

static int man_name_exact(const uint8_t *data, size_t size, const char *name)
{
    size_t name_size = strlen(name);
    return data && size == name_size && memcmp(data, name, size) == 0;
}

static int man_next_line(const uint8_t *data, size_t size, size_t *position,
                         ManLine *line)
{
    size_t start;
    if (!data || !position || !line || *position >= size) return 0;
    start = *position;
    while (*position < size && data[*position] != '\r' && data[*position] != '\n')
        ++*position;
    line->data = data + start;
    line->size = *position - start;
    if (*position < size && data[*position] == '\r') ++*position;
    if (*position < size && data[*position] == '\n') ++*position;
    return 1;
}

static void man_trim_argument(const uint8_t **data, size_t *size)
{
    while (*size && (**data == ' ' || **data == '\t')) {
        ++*data;
        --*size;
    }
    while (*size && ((*data)[*size - 1u] == ' ' || (*data)[*size - 1u] == '\t'))
        --*size;
    if (*size >= 2u && (*data)[0] == '"' && (*data)[*size - 1u] == '"') {
        ++*data;
        *size -= 2u;
    }
}

static void man_first_argument(const uint8_t *data, size_t size,
                               const uint8_t **out_data, size_t *out_size,
                               size_t *out_consumed)
{
    size_t position = 0u;
    size_t start;
    uint8_t quote = 0u;
    while (position < size && (data[position] == ' ' || data[position] == '\t'))
        ++position;
    start = position;
    if (position < size && data[position] == '"') quote = data[position++];
    while (position < size) {
        if (quote) {
            if (data[position++] == quote) break;
        } else if (data[position] == ' ' || data[position] == '\t') {
            break;
        } else {
            ++position;
        }
    }
    *out_data = data + start;
    *out_size = position - start;
    man_trim_argument(out_data, out_size);
    if (out_consumed) *out_consumed = position;
}

static int man_render_heading(ManParser *parser, unsigned level,
                              const uint8_t *text, size_t size)
{
    char open_tag[16];
    char close_tag[16];
    int open_size;
    int close_size;
    if (!man_close_paragraph(parser)) return 0;
    if (parser->in_preformatted) {
        if (!man_literal(parser, "</pre>")) return 0;
        parser->in_preformatted = 0;
    }
    if (level < 1u) level = 1u;
    if (level > 6u) level = 6u;
    open_size = snprintf(open_tag, sizeof(open_tag), "<h%u>", level);
    close_size = snprintf(close_tag, sizeof(close_tag), "</h%u>", level);
    if (open_size < 0 || close_size < 0 ||
        (size_t)open_size >= sizeof(open_tag) ||
        (size_t)close_size >= sizeof(close_tag) ||
        !man_append(parser, open_tag, (size_t)open_size) ||
        !man_html_text(parser, text, size) ||
        !man_append(parser, close_tag, (size_t)close_size)) return 0;
    parser->saw_content = 1;
    if (level > 1u) parser->saw_section = 1;
    return 1;
}

static int man_render_regular_line(ManParser *parser, const ManLine *line)
{
    const uint8_t *text = line->data;
    size_t size = line->size;
    while (size && (text[0] == ' ' || text[0] == '\t')) {
        ++text;
        --size;
    }
    while (size && (text[size - 1u] == ' ' || text[size - 1u] == '\t')) --size;
    if (!size) return man_close_paragraph(parser);
    if (parser->in_preformatted) {
        return man_html_escape(parser, line->data, line->size) &&
               man_literal(parser, "\n");
    }
    if (parser->expect_term) {
        if (!man_close_paragraph(parser) || !man_open_paragraph(parser) ||
            !man_literal(parser, "<strong>") || !man_html_text(parser, text, size) ||
            !man_literal(parser, "</strong><br>")) return 0;
        parser->expect_term = 0;
        parser->term_definition = 1;
        parser->paragraph_has_text = 1;
        parser->saw_content = 1;
        return 1;
    }
    if (!man_open_paragraph(parser)) return 0;
    if (parser->paragraph_has_text && !man_literal(parser, " ")) return 0;
    if (!man_html_text(parser, text, size)) return 0;
    parser->paragraph_has_text = 1;
    parser->saw_content = 1;
    return 1;
}

static int man_process_macro(ManParser *parser, const ManLine *line)
{
    size_t name_start = 1u;
    size_t name_end;
    size_t argument_start;
    size_t argument_size;
    const uint8_t *argument;
    const uint8_t *first_arg;
    size_t first_size;
    size_t consumed;
    while (name_start < line->size && (line->data[name_start] == ' ' ||
           line->data[name_start] == '\t')) ++name_start;
    if (name_start >= line->size) return 1;
    if (line->data[name_start] == '"') return 1;
    name_end = name_start;
    while (name_end < line->size &&
           ((line->data[name_end] >= 'A' && line->data[name_end] <= 'Z') ||
            (line->data[name_end] >= 'a' && line->data[name_end] <= 'z'))) ++name_end;
    if (name_end == name_start) return man_render_regular_line(parser, line);
    argument_start = name_end;
    argument = line->data + argument_start;
    argument_size = line->size - argument_start;
    while (argument_size && (*argument == ' ' || *argument == '\t')) {
        ++argument;
        --argument_size;
    }
    if (man_name_exact(line->data + name_start, name_end - name_start, "br"))
        return man_open_paragraph(parser) && man_literal(parser, "<br>");
    if (man_name_equal(line->data + name_start, name_end - name_start, "TH")) {
        const uint8_t *title;
        const uint8_t *section;
        size_t title_size;
        size_t section_size;
        size_t rest;
        man_first_argument(argument, argument_size, &title, &title_size, &rest);
        if (!title_size) return 1;
        man_first_argument(argument + rest, argument_size - rest,
                           &section, &section_size, NULL);
        if (!man_render_heading(parser, 1u, title, title_size)) return 0;
        if (section_size && (!man_literal(parser, "<p><small>Section ") ||
            !man_html_text(parser, section, section_size) ||
            !man_literal(parser, "</small></p>"))) return 0;
        parser->saw_title = 1;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "SH") ||
        man_name_equal(line->data + name_start, name_end - name_start, "SS")) {
        unsigned level = man_name_equal(line->data + name_start,
                                       name_end - name_start, "SH") ? 2u : 3u;
        man_trim_argument(&argument, &argument_size);
        return man_render_heading(parser, level, argument, argument_size);
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "PP") ||
        man_name_equal(line->data + name_start, name_end - name_start, "P") ||
        man_name_equal(line->data + name_start, name_end - name_start, "LP") ||
        man_name_equal(line->data + name_start, name_end - name_start, "SP")) {
        parser->expect_term = 0;
        return man_close_paragraph(parser);
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "TP")) {
        if (!man_close_paragraph(parser)) return 0;
        parser->expect_term = 1;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "IP")) {
        if (!man_close_paragraph(parser) || !man_open_paragraph(parser)) return 0;
        man_first_argument(argument, argument_size, &first_arg, &first_size, &consumed);
        if (first_size && !man_html_text(parser, first_arg, first_size)) return 0;
        if (!man_literal(parser, first_size ? " " : "• ")) return 0;
        argument += consumed;
        argument_size -= consumed;
        man_trim_argument(&argument, &argument_size);
        if (argument_size && !man_html_text(parser, argument, argument_size)) return 0;
        parser->paragraph_has_text = 1;
        parser->saw_content = 1;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "B") ||
        man_name_equal(line->data + name_start, name_end - name_start, "I") ||
        man_name_equal(line->data + name_start, name_end - name_start, "SM") ||
        man_name_equal(line->data + name_start, name_end - name_start, "SB")) {
        const char *wrapper = "<strong>";
        if (man_name_equal(line->data + name_start, name_end - name_start, "I"))
            wrapper = "<em>";
        else if (man_name_equal(line->data + name_start, name_end - name_start, "SM"))
            wrapper = "<small>";
        else if (man_name_equal(line->data + name_start, name_end - name_start, "SB"))
            wrapper = "<small><strong>";
        if (!man_close_paragraph(parser) || !man_open_paragraph(parser) ||
            !man_render_inline(parser, argument, argument_size, wrapper) ||
            !man_close_paragraph(parser)) return 0;
        parser->saw_content = 1;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "BR") ||
        man_name_equal(line->data + name_start, name_end - name_start, "IR") ||
        man_name_equal(line->data + name_start, name_end - name_start, "BI") ||
        man_name_equal(line->data + name_start, name_end - name_start, "IB") ||
        man_name_equal(line->data + name_start, name_end - name_start, "RB") ||
        man_name_equal(line->data + name_start, name_end - name_start, "RI")) {
        int first_bold = line->data[name_start] == 'B' || line->data[name_start] == 'b';
        int first_italic = line->data[name_start] == 'I' || line->data[name_start] == 'i';
        if (!man_close_paragraph(parser) || !man_open_paragraph(parser)) return 0;
        man_first_argument(argument, argument_size, &first_arg, &first_size, &consumed);
        if (first_size && !man_render_inline(parser, first_arg, first_size,
                first_bold ? "<strong>" : first_italic ? "<em>" : NULL)) return 0;
        argument += consumed;
        argument_size -= consumed;
        man_trim_argument(&argument, &argument_size);
        if (argument_size && !man_render_inline(parser, argument, argument_size,
                first_bold ? NULL : first_italic ? "<strong>" : "<em>")) return 0;
        if (!man_close_paragraph(parser)) return 0;
        parser->saw_content = 1;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "NF") ||
        man_name_equal(line->data + name_start, name_end - name_start, "EX")) {
        if (!man_close_paragraph(parser)) return 0;
        if (!parser->in_preformatted && !man_literal(parser, "<pre>")) return 0;
        parser->in_preformatted = 1;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "FI") ||
        man_name_equal(line->data + name_start, name_end - name_start, "EE")) {
        if (parser->in_preformatted && !man_literal(parser, "</pre>")) return 0;
        parser->in_preformatted = 0;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "RS")) {
        if (!man_close_paragraph(parser) || parser->indent_depth >= MAN_MAX_INDENT_DEPTH)
            return 0;
        if (!man_literal(parser, "<blockquote>")) return 0;
        ++parser->indent_depth;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "RE")) {
        if (!man_close_paragraph(parser)) return 0;
        if (parser->indent_depth && !man_literal(parser, "</blockquote>")) return 0;
        if (parser->indent_depth) --parser->indent_depth;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "DE") ||
        man_name_equal(line->data + name_start, name_end - name_start, "AM") ||
        man_name_equal(line->data + name_start, name_end - name_start, "IG")) {
        parser->in_macro_definition = 1;
        return 1;
    }
    if (man_name_equal(line->data + name_start, name_end - name_start, "SO") ||
        man_name_equal(line->data + name_start, name_end - name_start, "MSO")) {
        parser->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    parser->saw_ignored_macro = 1;
    return 1;
}

static int man_parse_document(const void *input, size_t input_size,
                              QwLayoutDocument *out_document,
                              QwConvertReport *report,
                              int *out_ignored_unsupported_macros)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        "@page{size:A4;margin:18mm}body{font-family:serif;font-size:10pt;"
        "line-height:1.4;color:#202124}h1,h2,h3{font-family:sans-serif;"
        "line-height:1.2}pre{font-family:monospace;white-space:pre-wrap}"
        "blockquote{margin-left:1.5em;border-left:1px solid #aaa;padding-left:1em}"
        "p{margin:.45em 0}</style></head><body>";
    static const char suffix[] = "</body></html>";
    const uint8_t *data = (const uint8_t *)input;
    size_t size = input_size;
    size_t position = 0u;
    ManParser parser;
    ManLine line;
    int rc = QW_STATUS_ERR_FORMAT;
    memset(&parser, 0, sizeof(parser));
    if (out_ignored_unsupported_macros)
        *out_ignored_unsupported_macros = 0;
    if (!input || !size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (size > MAN_MAX_INPUT_BYTES) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (!man_utf8_valid(data, size)) return QW_STATUS_ERR_DECODE;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu) {
        data += 3u;
        size -= 3u;
    }
    parser.html.limit = (size_t)MAN_MAX_OUTPUT_BYTES + 1u;
    if (!man_literal(&parser, prefix)) goto cleanup;
    while (man_next_line(data, size, &position, &line)) {
        if (parser.in_macro_definition) {
            if (line.size == 2u && line.data[0] == '.' && line.data[1] == '.')
                parser.in_macro_definition = 0;
            continue;
        }
        if (!line.size || man_line_is_blank(&line)) {
            if (!man_close_paragraph(&parser)) goto cleanup;
            continue;
        }
        if ((line.size >= 2u && line.data[0] == '.' && line.data[1] == '"') ||
            (line.size >= 3u && line.data[0] == '.' && line.data[1] == '\\' &&
             line.data[2] == '"') ||
            (line.size >= 2u && line.data[0] == '\'' && line.data[1] == '"'))
            continue;
        if (line.data[0] == '.' || line.data[0] == '\'') {
            if (!man_process_macro(&parser, &line)) goto cleanup;
        } else if (!man_render_regular_line(&parser, &line)) {
            goto cleanup;
        }
    }
    if (parser.in_preformatted && !man_literal(&parser, "</pre>")) goto cleanup;
    if (!man_close_paragraph(&parser)) goto cleanup;
    while (parser.indent_depth) {
        if (!man_literal(&parser, "</blockquote>")) goto cleanup;
        --parser.indent_depth;
    }
    if (!parser.saw_title || !parser.saw_section || !parser.saw_content ||
        !man_literal(&parser, suffix)) goto cleanup;
    out_document->html_utf8 = (const uint8_t *)parser.html.data;
    out_document->html_size = parser.html.size;
    out_document->context = parser.html.data;
    out_document->dispose = free;
    parser.html.data = NULL;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "Man page opened through a bounded UTF-8 roff subset%s%s",
            parser.saw_ignored_macro ? "; unsupported macros were skipped" : "",
            "; roff tables, equations, and external macro packages are not expanded");
    }
    if (out_ignored_unsupported_macros)
        *out_ignored_unsupported_macros = parser.saw_ignored_macro;
    rc = QW_STATUS_OK;

cleanup:
    free(parser.html.data);
    if (rc != QW_STATUS_OK && parser.error) rc = parser.error;
    if (rc != QW_STATUS_OK && report && !report->note[0]) {
        (void)snprintf(report->note, sizeof(report->note),
            "invalid or unsupported man-page input; .so/.mso includes are not followed");
    }
    return rc;
}

#ifndef QUIREWISE_STANDALONE_MAN_PARSER
static int man_probe(const void *input, size_t input_size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t size = input_size;
    size_t position = 0u;
    size_t line_count = 0u;
    int saw_title = 0;
    int saw_section = 0;
    ManLine line;
    if (!input || !size || size > MAN_MAX_INPUT_BYTES ||
        !man_utf8_valid(data, size)) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu) {
        data += 3u;
        size -= 3u;
    }
    while (line_count < 64u && man_next_line(data, size, &position, &line)) {
        ++line_count;
        if (line.size >= 4u && line.data[0] == '.' &&
            (line.data[1] == 'T' || line.data[1] == 't') &&
            (line.data[2] == 'H' || line.data[2] == 'h') &&
            (line.data[3] == ' ' || line.data[3] == '\t')) saw_title = 1;
        if (line.size >= 4u && line.data[0] == '.' &&
            (line.data[1] == 'S' || line.data[1] == 's') &&
            (line.data[2] == 'H' || line.data[2] == 'h') &&
            (line.data[3] == ' ' || line.data[3] == '\t')) saw_section = 1;
        if (saw_title && saw_section) return 1;
    }
    return 0;
}

static int man_open_layout(const void *data, size_t size,
                           QwInputFormat input_format,
                           const QwConvertOptions *options,
                           QwLayoutDocument *out_document,
                           QwConvertReport *report)
{
    QwManDocument parsed = {0};
    QwManStatus status;
    (void)options;
    if (!out_document || input_format != QW_FORMAT_MAN)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    status = qw_man_parse_html(data, size, &parsed);
    if (status != QW_MAN_OK) {
        if (report && !report->note[0]) {
            (void)snprintf(report->note, sizeof(report->note),
                "invalid or unsupported man-page input; .so/.mso includes are not followed");
        }
        switch (status) {
        case QW_MAN_ERR_INVALID_ARGUMENT: return QW_STATUS_ERR_INVALID_ARGUMENT;
        case QW_MAN_ERR_INVALID_UTF8: return QW_STATUS_ERR_DECODE;
        case QW_MAN_ERR_OUT_OF_MEMORY: return QW_STATUS_ERR_OUT_OF_MEMORY;
        case QW_MAN_ERR_RESOURCE_LIMIT: return QW_STATUS_ERR_RESOURCE_LIMIT;
        default: return QW_STATUS_ERR_FORMAT;
        }
    }
    out_document->html_utf8 = (const uint8_t *)parsed.html_utf8;
    out_document->html_size = parsed.html_size;
    out_document->context = parsed.html_utf8;
    out_document->dispose = free;
    parsed.html_utf8 = NULL;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "Man page opened through a bounded UTF-8 roff subset%s%s",
            parsed.skipped_unsupported_macros
                ? "; unsupported macros were skipped" : "",
            "; roff tables, equations, and external macro packages are not expanded");
    }
    qw_man_document_free(&parsed);
    return QW_STATUS_OK;
}

const QwFormatDescriptor qw_format_man = {
    .id = QW_FORMAT_MAN,
    .name = "man page",
    .probe = man_probe,
    .open_layout = man_open_layout,
    .max_input_bytes = MAN_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};
#endif

int qw_man_abi_version(void)
{
    return QW_MAN_ABI_VERSION;
}

static QwManStatus man_public_status(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_MAN_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_MAN_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_DECODE: return QW_MAN_ERR_INVALID_UTF8;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_MAN_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_MAN_ERR_RESOURCE_LIMIT;
    default: return QW_MAN_ERR_FORMAT;
    }
}

QwManStatus qw_man_parse_html(const void *data, size_t size,
                              QwManDocument *out_document)
{
    QwLayoutDocument parsed = {0};
    int skipped_unsupported_macros = 0;
    int status;
    if (!out_document) return QW_MAN_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    status = man_parse_document(data, size, &parsed, NULL,
                                &skipped_unsupported_macros);
    if (status != QW_STATUS_OK) return man_public_status(status);
    out_document->html_utf8 = (char *)parsed.context;
    out_document->html_size = parsed.html_size;
    out_document->skipped_unsupported_macros = skipped_unsupported_macros;
    return QW_MAN_OK;
}

void qw_man_document_free(QwManDocument *document)
{
    if (!document) return;
    free(document->html_utf8);
    memset(document, 0, sizeof(*document));
}

const char *qw_man_status_string(QwManStatus status)
{
    switch (status) {
    case QW_MAN_OK: return "ok";
    case QW_MAN_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_MAN_ERR_INVALID_UTF8: return "invalid UTF-8";
    case QW_MAN_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_MAN_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    case QW_MAN_ERR_FORMAT: return "invalid or unsupported man page";
    default: return "unknown status";
    }
}

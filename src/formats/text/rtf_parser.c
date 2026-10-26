#include "../../core/format_registry.h"
#include "../../core/text_buffer.h"
#include "rtf_parser.h"
#include "rtf_picture_parser.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <iconv.h>
#endif
enum {

    QW_RTF_HTML_LIMIT = 512 * 1024 * 1024,
    QW_RTF_GROUP_LIMIT = 512,
    QW_RTF_ANSI_CAPACITY = 65536
};

typedef struct QwRtfState {
    int bold;
    int italic;
    int underline;
    int strike;
    int hidden;
    int suppress;
    int font_size;
    int align;
    int codepage;
    int uc_skip;
} QwRtfState;

typedef struct QwRtfFrame {
    QwRtfState state;
    int first_control;
    int starred;
    int picture_container;
} QwRtfFrame;

typedef struct QwRtfParser {
    QwTextBuffer html;
    QwRtfState state;
    QwRtfFrame frames[QW_RTF_GROUP_LIMIT];
    size_t depth;
    unsigned char ansi[QW_RTF_ANSI_CAPACITY];
    size_t ansi_size;
    size_t fallback_remaining;
    unsigned high_surrogate;
    int paragraph_open;
    int saw_text;
    int root_closed;
    int active_bold;
    int active_italic;
    int active_underline;
    int active_strike;
    int active_font_size;
    int saw_table;
    int saw_picture;
    int rendered_picture;
    int omitted_picture;
    int saw_object;
    int saw_header_footer;
    int saw_footnote;
} QwRtfParser;

static int rtf_prepare_text(QwRtfParser *parser);

static int rtf_append_literal(QwRtfParser *parser, const char *text)
{
    return qw_text_buffer_append(&parser->html, text, strlen(text));
}

static int rtf_append_codepoint(QwRtfParser *parser, unsigned cp)
{
    char bytes[4];
    size_t count;

    if (cp <= 0x7fu) {
        bytes[0] = (char)cp;
        count = 1u;
    } else if (cp <= 0x7ffu) {
        bytes[0] = (char)(0xc0u | (cp >> 6));
        bytes[1] = (char)(0x80u | (cp & 0x3fu));
        count = 2u;
    } else if (cp <= 0xffffu && !(cp >= 0xd800u && cp <= 0xdfffu)) {
        bytes[0] = (char)(0xe0u | (cp >> 12));
        bytes[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        bytes[2] = (char)(0x80u | (cp & 0x3fu));
        count = 3u;
    } else if (cp <= 0x10ffffu) {
        bytes[0] = (char)(0xf0u | (cp >> 18));
        bytes[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
        bytes[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        bytes[3] = (char)(0x80u | (cp & 0x3fu));
        count = 4u;
    } else {
        return rtf_append_literal(parser, "\xef\xbf\xbd");
    }
    return qw_text_buffer_append(&parser->html, bytes, count);
}

static int rtf_emit_utf8(QwRtfParser *parser, const char *data, size_t size)
{
    size_t i = 0;
    if (!rtf_prepare_text(parser)) return 0;
    if (size && !parser->state.suppress && !parser->state.hidden)
        parser->saw_text = 1;
    while (i < size) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '&') {
            if (!rtf_append_literal(parser, "&amp;")) return 0;
            ++i;
        } else if (ch == '<') {
            if (!rtf_append_literal(parser, "&lt;")) return 0;
            ++i;
        } else if (ch == '>') {
            if (!rtf_append_literal(parser, "&gt;")) return 0;
            ++i;
        } else if (ch < 0x20u && ch != '\t' && ch != '\r' && ch != '\n') {
            if (!rtf_append_literal(parser, " ")) return 0;
            ++i;
        } else {
            size_t run = i + 1u;
            while (run < size && data[run] != '&' && data[run] != '<' &&
                   data[run] != '>' &&
                   (unsigned char)data[run] >= 0x20u) {
                ++run;
            }
            if (!qw_text_buffer_append(&parser->html, data + i, run - i)) return 0;
            i = run;
        }
    }
    return 1;
}

static int rtf_ansi_safe_prefix(const unsigned char *data, size_t size, int cp)
{
    size_t prefix = size;
    if (!size) return 0;
    if (cp == 65001) {
        size_t start = size;
        size_t continuations = 0;
        while (start > 0u && (data[start - 1u] & 0xc0u) == 0x80u &&
               continuations < 3u) {
            --start;
            ++continuations;
        }
        if (start > 0u) {
            unsigned char lead = data[start - 1u];
            size_t expected = lead >= 0xf0u && lead <= 0xf4u ? 4u :
                              lead >= 0xe0u && lead <= 0xefu ? 3u :
                              lead >= 0xc2u && lead <= 0xdfu ? 2u : 1u;
            if (expected > continuations + 1u) prefix = start - 1u;
        } else if (continuations) {
            prefix = 0u;
        }
    } else if ((cp == 932 || cp == 936 || cp == 949 || cp == 950) &&
               size > 0u) {
        unsigned char last = data[size - 1u];
        int lead = cp == 932
            ? ((last >= 0x81u && last <= 0x9fu) ||
               (last >= 0xe0u && last <= 0xfcu))
            : (last >= 0x81u && last <= 0xfeu);
        if (lead) prefix = size - 1u;
    }
    return (int)prefix;
}

static int rtf_decode_ansi(QwRtfParser *parser,
                           const unsigned char *data,
                           size_t size)
{
    if (!size || parser->state.suppress || parser->state.hidden) return 1;
#if defined(_WIN32)
    {
        int wide_count;
        int utf8_count;
        wchar_t *wide;
        char *utf8;
        UINT codepage = parser->state.codepage > 0
            ? (UINT)parser->state.codepage : 1252u;
        if (size > (size_t)INT_MAX) return 0;
        wide_count = MultiByteToWideChar(codepage, 0, (const char *)data,
                                         (int)size, NULL, 0);
        if (wide_count <= 0 ||
            (size_t)wide_count > SIZE_MAX / sizeof(wchar_t)) return 0;
        wide = (wchar_t *)malloc((size_t)wide_count * sizeof(wchar_t));
        if (!wide) return 0;
        if (MultiByteToWideChar(codepage, 0, (const char *)data, (int)size,
                                wide, wide_count) != wide_count) {
            free(wide);
            return 0;
        }
        utf8_count = WideCharToMultiByte(CP_UTF8, 0, wide, wide_count,
                                         NULL, 0, NULL, NULL);
        if (utf8_count <= 0) {
            free(wide);
            return 0;
        }
        utf8 = (char *)malloc((size_t)utf8_count);
        if (!utf8) {
            free(wide);
            return 0;
        }
        if (WideCharToMultiByte(CP_UTF8, 0, wide, wide_count,
                                utf8, utf8_count, NULL, NULL) != utf8_count) {
            free(utf8);
            free(wide);
            return 0;
        }
        free(wide);
        {
            int ok = rtf_emit_utf8(parser, utf8, (size_t)utf8_count);
            free(utf8);
            return ok;
        }
    }
#else
    {
        char codepage_name[24];
        iconv_t converter;
        size_t capacity;
        char *utf8;
        char *input;
        char *output;
        size_t input_left = size;
        size_t output_left;
        size_t result;
        if (parser->state.codepage == 10000) {
            strcpy(codepage_name, "MACINTOSH");
        } else if (parser->state.codepage == 65001) {
            strcpy(codepage_name, "UTF-8");
        } else {
            snprintf(codepage_name, sizeof(codepage_name), "CP%d",
                     parser->state.codepage > 0 ? parser->state.codepage : 1252);
        }
        converter = iconv_open("UTF-8", codepage_name);
        if (converter == (iconv_t)-1) return 0;
        if (size > (SIZE_MAX - 16u) / 4u) {
            iconv_close(converter);
            return 0;
        }
        capacity = size * 4u + 16u;
        utf8 = (char *)malloc(capacity);
        if (!utf8) {
            iconv_close(converter);
            return 0;
        }
        input = (char *)(uintptr_t)data;
        output = utf8;
        output_left = capacity;
        errno = 0;
        result = iconv(converter, &input, &input_left, &output, &output_left);
        iconv_close(converter);
        if (result == (size_t)-1 || input_left != 0u) {
            free(utf8);
            return 0;
        }
        result = (size_t)(output - utf8);
        {
            int ok = rtf_emit_utf8(parser, utf8, result);
            free(utf8);
            return ok;
        }
    }
#endif
}

static int rtf_flush_ansi(QwRtfParser *parser, int final)
{
    size_t prefix;
    if (!parser->ansi_size) return 1;
    if (parser->state.suppress || parser->state.hidden) {
        parser->ansi_size = 0u;
        return 1;
    }
    prefix = final ? parser->ansi_size :
        (size_t)rtf_ansi_safe_prefix(parser->ansi, parser->ansi_size,
                                     parser->state.codepage);
    if (!prefix) return 1;
    if (!rtf_decode_ansi(parser, parser->ansi, prefix)) return 0;
    parser->ansi_size -= prefix;
    if (parser->ansi_size) {
        memmove(parser->ansi, parser->ansi + prefix, parser->ansi_size);
    }
    return 1;
}

static int rtf_ensure_paragraph(QwRtfParser *parser)
{
    const char *alignment = parser->state.align == 1 ? "center" :
                            parser->state.align == 2 ? "right" :
                            parser->state.align == 3 ? "justify" : "left";
    char opening[96];
    if (parser->paragraph_open) return 1;
    if (snprintf(opening, sizeof(opening), "<p style=\"text-align:%s\">",
                 alignment) < 0 || !rtf_append_literal(parser, opening)) return 0;
    parser->paragraph_open = 1;
    return 1;
}

static int rtf_close_inline(QwRtfParser *parser, int bold, int italic,
                            int underline, int strike, int font_size)
{
    if (font_size && !rtf_append_literal(parser, "</span>")) return 0;
    if (strike && !rtf_append_literal(parser, "</s>")) return 0;
    if (underline && !rtf_append_literal(parser, "</u>")) return 0;
    if (italic && !rtf_append_literal(parser, "</em>")) return 0;
    if (bold && !rtf_append_literal(parser, "</strong>")) return 0;
    return 1;
}

static int rtf_open_inline(QwRtfParser *parser, const QwRtfState *state)
{
    char style[64];
    int half_points = state->font_size;
    if (state->bold && !rtf_append_literal(parser, "<strong>")) return 0;
    if (state->italic && !rtf_append_literal(parser, "<em>")) return 0;
    if (state->underline && !rtf_append_literal(parser, "<u>")) return 0;
    if (state->strike && !rtf_append_literal(parser, "<s>")) return 0;
    if (half_points != 24) {
        int whole = half_points / 2;
        int fraction = (half_points & 1) ? 5 : 0;
        if (snprintf(style, sizeof(style), "<span style=\"font-size:%d.%dpt\">",
                     whole, fraction) < 0 || !rtf_append_literal(parser, style)) return 0;
    }
    return 1;
}

static int rtf_sync_inline(QwRtfParser *parser,
                           const QwRtfState *active,
                           int *active_bold,
                           int *active_italic,
                           int *active_underline,
                           int *active_strike,
                           int *active_font_size)
{
    int font_size = parser->state.font_size == 24 ? 0 : parser->state.font_size;
    int changed = *active_bold != parser->state.bold ||
                  *active_italic != parser->state.italic ||
                  *active_underline != parser->state.underline ||
                  *active_strike != parser->state.strike ||
                  *active_font_size != font_size;
    (void)active;
    if (!changed) return 1;
    if (!rtf_close_inline(parser, *active_bold, *active_italic,
                          *active_underline, *active_strike,
                          *active_font_size)) return 0;
    if (!rtf_open_inline(parser, &parser->state)) return 0;
    *active_bold = parser->state.bold;
    *active_italic = parser->state.italic;
    *active_underline = parser->state.underline;
    *active_strike = parser->state.strike;
    *active_font_size = font_size;
    parser->active_bold = *active_bold;
    parser->active_italic = *active_italic;
    parser->active_underline = *active_underline;
    parser->active_strike = *active_strike;
    parser->active_font_size = *active_font_size;
    return 1;
}

static int rtf_prepare_text(QwRtfParser *parser)
{
    if (parser->state.suppress || parser->state.hidden) return 1;
    if (!rtf_ensure_paragraph(parser)) return 0;
    return rtf_sync_inline(parser, NULL, &parser->active_bold,
                           &parser->active_italic,
                           &parser->active_underline,
                           &parser->active_strike,
                           &parser->active_font_size);
}

static int rtf_emit_text_byte(QwRtfParser *parser, unsigned char value)
{
    if (parser->state.suppress || parser->state.hidden) return 1;
    if (parser->ansi_size == sizeof(parser->ansi) &&
        !rtf_flush_ansi(parser, 0)) return 0;
    if (parser->ansi_size == sizeof(parser->ansi)) return 0;
    parser->ansi[parser->ansi_size++] = value;
    if (parser->ansi_size >= sizeof(parser->ansi) - 8u &&
        !rtf_flush_ansi(parser, 0)) return 0;
    return 1;
}

static int rtf_emit_codepoint(QwRtfParser *parser, unsigned cp,
                              int *active_bold,
                              int *active_italic,
                              int *active_underline,
                              int *active_strike,
                              int *active_font_size)
{
    QwRtfState active;
    if (parser->state.suppress || parser->state.hidden) return 1;
    if (!rtf_flush_ansi(parser, 1)) return 0;
    if (!rtf_ensure_paragraph(parser)) return 0;
    active.bold = *active_bold;
    active.italic = *active_italic;
    active.underline = *active_underline;
    active.strike = *active_strike;
    active.font_size = *active_font_size;
    if (!rtf_sync_inline(parser, &active, active_bold, active_italic,
                         active_underline, active_strike,
                         active_font_size)) return 0;
    parser->saw_text = 1;
    return rtf_append_codepoint(parser, cp);
}

static int rtf_emit_u16(QwRtfParser *parser, unsigned unit,
                        int *active_bold,
                        int *active_italic,
                        int *active_underline,
                        int *active_strike,
                        int *active_font_size)
{
    if (unit >= 0xd800u && unit <= 0xdbffu) {
        if (parser->high_surrogate &&
            !rtf_emit_codepoint(parser, 0xfffdu, active_bold, active_italic,
                                active_underline, active_strike,
                                active_font_size)) return 0;
        parser->high_surrogate = unit;
        return 1;
    }
    if (unit >= 0xdc00u && unit <= 0xdfffu) {
        unsigned cp;
        if (parser->high_surrogate) {
            cp = 0x10000u + ((parser->high_surrogate - 0xd800u) << 10) +
                 (unit - 0xdc00u);
            parser->high_surrogate = 0u;
        } else {
            cp = 0xfffdu;
        }
        return rtf_emit_codepoint(parser, cp, active_bold, active_italic,
                                  active_underline, active_strike,
                                  active_font_size);
    }
    if (parser->high_surrogate) {
        parser->high_surrogate = 0u;
        if (!rtf_emit_codepoint(parser, 0xfffdu, active_bold, active_italic,
                                active_underline, active_strike,
                                active_font_size)) return 0;
    }
    return rtf_emit_codepoint(parser, unit, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size);
}

static int rtf_close_paragraph(QwRtfParser *parser,
                               int *active_bold,
                               int *active_italic,
                               int *active_underline,
                               int *active_strike,
                               int *active_font_size)
{
    if (!rtf_flush_ansi(parser, 1)) return 0;
    if (parser->paragraph_open) {
        if (!rtf_close_inline(parser, *active_bold, *active_italic,
                              *active_underline, *active_strike,
                              *active_font_size) ||
            !rtf_append_literal(parser, "</p>")) return 0;
        parser->paragraph_open = 0;
        *active_bold = *active_italic = *active_underline = *active_strike = 0;
        *active_font_size = 0;
        parser->active_bold = parser->active_italic =
            parser->active_underline = parser->active_strike =
            parser->active_font_size = 0;
    } else if (parser->saw_text) {
        if (!rtf_append_literal(parser, "<p><br/></p>")) return 0;
    }
    return 1;
}

static int rtf_is_destination(const char *word)
{
    static const char *const destinations[] = {
        "fonttbl", "colortbl", "stylesheet", "info", "pict", "object",
        "header", "headerl", "headerr", "headerf", "footer", "footerl",
        "footerr", "footerf", "footnote", "annotation", "atnauthor",
        "atnid", "generator", "xmlnstbl", "datastore", "themedata",
        "colorschememapping", "listtable", "listoverridetable", "listtext",
        "revtbl", "rsidtbl", "latentstyles", "mmathpr", "nonshppict",
        "shppict", "blipuid", "private", "userprops", "filetbl", "*",
        "fldinst", "datafield", "formfield", "ffname", "fftype", "fftext"
    };
    size_t i;
    for (i = 0; i < sizeof(destinations) / sizeof(destinations[0]); ++i) {
        if (strcmp(word, destinations[i]) == 0) return 1;
    }
    return 0;
}

static int rtf_hex_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int rtf_is_word_char(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static int rtf_first_control(QwRtfParser *parser, const char *word)
{
    QwRtfFrame *frame;
    if (!parser->depth) return 0;
    frame = &parser->frames[parser->depth - 1u];
    if (!frame->first_control) return 0;
    frame->first_control = 0;
    if (strcmp(word, "shppict") == 0) frame->picture_container = 1;
    else if (strcmp(word, "nonshppict") == 0) frame->picture_container = 2;
    if (frame->starred || rtf_is_destination(word)) {
        parser->state.suppress = 1;
        if (strcmp(word, "pict") == 0) {
            parser->saw_picture = 1;
            parser->omitted_picture = 1;
        } else if (strcmp(word, "shppict") == 0 ||
                   strcmp(word, "nonshppict") == 0) {
            parser->saw_picture = 1;
        }
        if (strcmp(word, "object") == 0) parser->saw_object = 1;
        if (strcmp(word, "header") == 0 || strcmp(word, "headerl") == 0 ||
            strcmp(word, "headerr") == 0 || strcmp(word, "headerf") == 0 ||
            strcmp(word, "footer") == 0 || strcmp(word, "footerl") == 0 ||
            strcmp(word, "footerr") == 0 || strcmp(word, "footerf") == 0)
            parser->saw_header_footer = 1;
        if (strcmp(word, "footnote") == 0) parser->saw_footnote = 1;
        return 1;
    }
    return 0;
}

static int rtf_picture_context_allowed(const QwRtfParser *parser)
{
    size_t i;
    if (!parser->state.suppress) return 1;
    for (i = parser->depth; i > 0u; --i) {
        if (parser->frames[i - 1u].picture_container == 1) return 1;
    }
    return 0;
}

static void rtf_set_codepage(QwRtfState *state, int codepage)
{
    if (codepage >= 1 && codepage <= 65535) state->codepage = codepage;
}

static int rtf_emit_special(QwRtfParser *parser,
                            unsigned cp,
                            int *active_bold,
                            int *active_italic,
                            int *active_underline,
                            int *active_strike,
                            int *active_font_size)
{
    if (parser->fallback_remaining) {
        --parser->fallback_remaining;
        return 1;
    }
    if (parser->high_surrogate) {
        parser->high_surrogate = 0u;
        if (!rtf_emit_codepoint(parser, 0xfffdu, active_bold, active_italic,
                                active_underline, active_strike,
                                active_font_size)) return 0;
    }
    return rtf_emit_codepoint(parser, cp, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size);
}

static int rtf_process_control(QwRtfParser *parser,
                              const char *word,
                              int has_parameter,
                              int parameter,
                              int *active_bold,
                              int *active_italic,
                              int *active_underline,
                              int *active_strike,
                              int *active_font_size)
{
    if (!rtf_flush_ansi(parser, 1)) return 0;
    int is_first = rtf_first_control(parser, word);
    if (!strcmp(word, "rtf") || !strcmp(word, "ansi") ||
        !strcmp(word, "mac") || !strcmp(word, "pc") ||
        !strcmp(word, "pca") || !strcmp(word, "deff") ||
        !strcmp(word, "viewkind") || !strcmp(word, "viewscale") ||
        !strcmp(word, "deflang") || !strcmp(word, "deflangfe") ||
        !strcmp(word, "lang")) {
        if (!strcmp(word, "mac")) rtf_set_codepage(&parser->state, 10000);
        else if (!strcmp(word, "pc")) rtf_set_codepage(&parser->state, 437);
        else if (!strcmp(word, "pca")) rtf_set_codepage(&parser->state, 850);
        else if (!strcmp(word, "ansi") && parser->state.codepage <= 0)
            rtf_set_codepage(&parser->state, 1252);
        (void)is_first;
        return 1;
    }
    if (!strcmp(word, "ansicpg")) {
        if (!rtf_flush_ansi(parser, 1)) return 0;
        if (has_parameter) rtf_set_codepage(&parser->state, parameter);
        return 1;
    }
    if (!strcmp(word, "uc")) {
        if (has_parameter && parameter >= 0 && parameter <= 16)
            parser->state.uc_skip = parameter;
        return 1;
    }
    if (!strcmp(word, "u")) {
        int unit;
        if (!has_parameter) return 1;
        unit = parameter < 0 ? 65536 + parameter : parameter;
        if (unit < 0 || unit > 65535) return 1;
        if (!rtf_emit_u16(parser, (unsigned)unit, active_bold, active_italic,
                          active_underline, active_strike,
                          active_font_size)) return 0;
        parser->fallback_remaining = (size_t)parser->state.uc_skip;
        return 1;
    }
    if (!strcmp(word, "bin")) {
        /* The caller handles the binary span so brace bytes remain opaque. */
        return 1;
    }
    if (parser->state.suppress) return 1;
    if (!strcmp(word, "b")) parser->state.bold = !has_parameter || parameter != 0;
    else if (!strcmp(word, "i")) parser->state.italic = !has_parameter || parameter != 0;
    else if (!strcmp(word, "ul")) parser->state.underline = !has_parameter || parameter != 0;
    else if (!strcmp(word, "ulnone") || !strcmp(word, "ul0")) parser->state.underline = 0;
    else if (!strcmp(word, "strike")) parser->state.strike = !has_parameter || parameter != 0;
    else if (!strcmp(word, "v")) parser->state.hidden = !has_parameter || parameter != 0;
    else if (!strcmp(word, "fs") && has_parameter) {
        if (parameter < 4) parameter = 4;
        if (parameter > 288) parameter = 288;
        parser->state.font_size = parameter;
    } else if (!strcmp(word, "plain")) {
        parser->state.bold = parser->state.italic = parser->state.underline =
            parser->state.strike = parser->state.hidden = 0;
        parser->state.font_size = 24;
    } else if (!strcmp(word, "ql")) parser->state.align = 0;
    else if (!strcmp(word, "qc")) parser->state.align = 1;
    else if (!strcmp(word, "qr")) parser->state.align = 2;
    else if (!strcmp(word, "qj")) parser->state.align = 3;
    else if (!strcmp(word, "pard")) parser->state.align = 0;
    else if (!strcmp(word, "par")) {
        if (!rtf_close_paragraph(parser, active_bold, active_italic,
                                 active_underline, active_strike,
                                 active_font_size)) return 0;
    } else if (!strcmp(word, "line")) {
        if (!rtf_flush_ansi(parser, 1) || !rtf_ensure_paragraph(parser)) return 0;
        if (!rtf_close_inline(parser, *active_bold, *active_italic,
                              *active_underline, *active_strike,
                              *active_font_size) ||
            !rtf_append_literal(parser, "<br/>")) return 0;
        *active_bold = *active_italic = *active_underline = *active_strike = 0;
        *active_font_size = 0;
        parser->active_bold = parser->active_italic =
            parser->active_underline = parser->active_strike =
            parser->active_font_size = 0;
    } else if (!strcmp(word, "tab")) {
        if (!rtf_emit_special(parser, 0x09u, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size)) return 0;
    } else if (!strcmp(word, "cell")) {
        parser->saw_table = 1;
        if (!rtf_emit_special(parser, 0x09u, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size)) return 0;
    } else if (!strcmp(word, "row")) {
        parser->saw_table = 1;
        if (!rtf_close_paragraph(parser, active_bold, active_italic,
                                 active_underline, active_strike,
                                 active_font_size)) return 0;
    } else if (!strcmp(word, "page")) {
        if (!rtf_close_paragraph(parser, active_bold, active_italic,
                                 active_underline, active_strike,
                                 active_font_size) ||
            !rtf_append_literal(parser,
                "<div style=\"break-before:page;page-break-before:always\"></div>")) return 0;
    } else if (!strcmp(word, "emdash")) {
        if (!rtf_emit_special(parser, 0x2014u, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size)) return 0;
    } else if (!strcmp(word, "endash")) {
        if (!rtf_emit_special(parser, 0x2013u, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size)) return 0;
    } else if (!strcmp(word, "bullet")) {
        if (!rtf_emit_special(parser, 0x2022u, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size)) return 0;
    } else if (!strcmp(word, "lquote")) {
        if (!rtf_emit_special(parser, 0x2018u, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size)) return 0;
    } else if (!strcmp(word, "rquote")) {
        if (!rtf_emit_special(parser, 0x2019u, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size)) return 0;
    } else if (!strcmp(word, "ldblquote")) {
        if (!rtf_emit_special(parser, 0x201cu, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size)) return 0;
    } else if (!strcmp(word, "rdblquote")) {
        if (!rtf_emit_special(parser, 0x201du, active_bold, active_italic,
                              active_underline, active_strike,
                              active_font_size)) return 0;
    }
    return 1;
}

int qw_rtf_probe(const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t i = 0;
    if (!bytes || size < 5u) return 0;
    if (size >= 3u && bytes[0] == 0xefu && bytes[1] == 0xbbu && bytes[2] == 0xbfu)
        i = 3u;
    while (i < size && (bytes[i] == ' ' || bytes[i] == '\t' ||
                        bytes[i] == '\r' || bytes[i] == '\n')) ++i;
    return i + 5u <= size && bytes[i] == '{' && bytes[i + 1u] == '\\' &&
           (bytes[i + 2u] == 'r' || bytes[i + 2u] == 'R') &&
           (bytes[i + 3u] == 't' || bytes[i + 3u] == 'T') &&
           (bytes[i + 4u] == 'f' || bytes[i + 4u] == 'F');
}

static int rtf_parse_internal(QwRtfParser *parser,
                     const unsigned char *data,
                     size_t size)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        "@page{size:A4;margin:20mm}body{font-family:sans-serif;font-size:11pt}"
        "p{white-space:pre-wrap;overflow-wrap:anywhere;margin:0 0 0.55em}"
        "</style></head><body>";
    static const char suffix[] = "</body></html>";
    size_t i = 0;
    int active_bold = 0;
    int active_italic = 0;
    int active_underline = 0;
    int active_strike = 0;
    int active_font_size = 0;
    int saw_root = 0;

    parser->html.limit = QW_RTF_HTML_LIMIT;
    parser->state.codepage = 1252;
    parser->state.uc_skip = 1;
    parser->state.font_size = 24;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        i = 3u;
    while (i < size && (data[i] == ' ' || data[i] == '\t' ||
                        data[i] == '\r' || data[i] == '\n')) ++i;
    if (!qw_text_buffer_append(&parser->html, prefix, sizeof(prefix) - 1u)) return QW_STATUS_ERR_OUT_OF_MEMORY;

    while (i < size) {
        unsigned char ch = data[i++];
        if (ch == '{') {
            if (!rtf_flush_ansi(parser, 1)) return QW_STATUS_ERR_DECODE;
            if (qw_rtf_picture_group_probe(data + i - 1u, size - i + 1u) &&
                rtf_picture_context_allowed(parser)) {
                size_t group_start = i - 1u;
                size_t group_size = 0u;
                int rendered = 0;
                int picture_rc;
                QwTextBuffer picture_html = {0};
                picture_html.limit = QW_RTF_HTML_LIMIT;
                picture_rc = qw_rtf_picture_group_parse(
                    data + group_start, size - group_start, &group_size,
                    &picture_html, &rendered);
                if (picture_rc != QW_STATUS_OK) {
                    free(picture_html.data);
                    return picture_rc;
                }
                if (!group_size || group_size > size - group_start) {
                    free(picture_html.data);
                    return QW_STATUS_ERR_FORMAT;
                }
                parser->saw_picture = 1;
                if (rendered) {
                    if (parser->paragraph_open &&
                        !rtf_close_paragraph(parser, &active_bold,
                            &active_italic, &active_underline,
                            &active_strike, &active_font_size)) {
                        free(picture_html.data);
                        return QW_STATUS_ERR_OUT_OF_MEMORY;
                    }
                    if (parser->html.size >= parser->html.limit ||
                        picture_html.size > parser->html.limit -
                            parser->html.size - 1u) {
                        free(picture_html.data);
                        return QW_STATUS_ERR_RESOURCE_LIMIT;
                    }
                    if (!qw_text_buffer_append(&parser->html,
                            picture_html.data, picture_html.size)) {
                        free(picture_html.data);
                        return QW_STATUS_ERR_OUT_OF_MEMORY;
                    }
                    parser->rendered_picture = 1;
                } else {
                    parser->omitted_picture = 1;
                }
                free(picture_html.data);
                i = group_start + group_size;
                continue;
            }
            if (parser->root_closed || parser->depth >= QW_RTF_GROUP_LIMIT)
                return QW_STATUS_ERR_FORMAT;
            parser->frames[parser->depth].state = parser->state;
            parser->frames[parser->depth].first_control = 1;
            parser->frames[parser->depth].starred = 0;
            ++parser->depth;
            saw_root = 1;
        } else if (ch == '}') {
            QwRtfFrame frame;
            if (!rtf_flush_ansi(parser, 1) || parser->depth == 0u)
                return QW_STATUS_ERR_FORMAT;
            frame = parser->frames[parser->depth - 1u];
            --parser->depth;
            parser->state = frame.state;
            if (!parser->depth) parser->root_closed = 1;
        } else if (ch == '\\') {
            unsigned char next;
            if (i >= size) return QW_STATUS_ERR_FORMAT;
            next = data[i++];
            if (next == '\\' || next == '{' || next == '}') {
                if (parser->fallback_remaining) --parser->fallback_remaining;
                else if (!rtf_emit_text_byte(parser, next)) return QW_STATUS_ERR_OUT_OF_MEMORY;
            } else if (next == '\'') {
                int high, low;
                unsigned char value;
                if (i + 1u >= size || (high = rtf_hex_value(data[i])) < 0 ||
                    (low = rtf_hex_value(data[i + 1u])) < 0) return QW_STATUS_ERR_FORMAT;
                i += 2u;
                if (parser->fallback_remaining) --parser->fallback_remaining;
                else {
                    value = (unsigned char)((high << 4) | low);
                    if (!rtf_emit_text_byte(parser, value)) return QW_STATUS_ERR_OUT_OF_MEMORY;
                }
            } else if (next == '*') {
                if (parser->depth) parser->frames[parser->depth - 1u].starred = 1;
            } else if (next == '~' || next == '_' || next == '-') {
                unsigned cp = next == '~' ? 0x00a0u : next == '_' ? 0x2011u : 0u;
                if (cp && !rtf_emit_special(parser, cp, &active_bold,
                    &active_italic, &active_underline, &active_strike,
                    &active_font_size)) return QW_STATUS_ERR_OUT_OF_MEMORY;
                else if (next == '-' && parser->fallback_remaining)
                    --parser->fallback_remaining;
            } else if (next == '\r' || next == '\n') {
                /* Source line wrapping is insignificant in RTF. */
            } else if (rtf_is_word_char(next)) {
                char word[40];
                size_t word_size = 0;
                int has_parameter = 0;
                int parameter = 0;
                int negative = 0;
                int parameter_overflow = 0;
                word[word_size++] = (char)next;
                while (i < size && rtf_is_word_char(data[i])) {
                    if (word_size + 1u < sizeof(word)) word[word_size++] = (char)data[i];
                    ++i;
                }
                word[word_size] = '\0';
                if (i < size && (data[i] == '-' || (data[i] >= '0' && data[i] <= '9'))) {
                    has_parameter = 1;
                    if (data[i] == '-') { negative = 1; ++i; }
                    while (i < size && data[i] >= '0' && data[i] <= '9') {
                        int digit = data[i] - '0';
                        if (parameter > (INT_MAX - digit) / 10)
                            parameter_overflow = 1;
                        else if (!parameter_overflow)
                            parameter = parameter * 10 + digit;
                        ++i;
                    }
                    if (parameter_overflow) return QW_STATUS_ERR_FORMAT;
                    if (negative) parameter = -parameter;
                }
                if (i < size && data[i] == ' ') ++i;
                if (!strcmp(word, "bin") && has_parameter) {
                    if (parameter < 0 || (size_t)parameter > size - i) return QW_STATUS_ERR_FORMAT;
                    i += (size_t)parameter;
                }
                if (!rtf_process_control(parser, word, has_parameter, parameter,
                    &active_bold, &active_italic, &active_underline,
                    &active_strike, &active_font_size)) return QW_STATUS_ERR_OUT_OF_MEMORY;
            } else {
                /* Non-alphabetic control symbols with no visible meaning are ignored. */
            }
        } else if (ch == '\r' || ch == '\n') {
            /* RTF source formatting whitespace is not document text. */
        } else if (parser->root_closed) {
            if (ch != ' ' && ch != '\t') return QW_STATUS_ERR_FORMAT;
        } else if (parser->fallback_remaining) {
            --parser->fallback_remaining;
        } else if (!parser->state.suppress && !parser->state.hidden) {
            if (!parser->depth) return QW_STATUS_ERR_FORMAT;
            if (parser->frames[parser->depth - 1u].first_control)
                parser->frames[parser->depth - 1u].first_control = 0;
            if (!rtf_emit_text_byte(parser, ch)) return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
    }
    if (!saw_root || !parser->root_closed || parser->depth != 0u ||
        !rtf_flush_ansi(parser, 1)) return QW_STATUS_ERR_FORMAT;
    if (parser->high_surrogate) {
        parser->high_surrogate = 0u;
        if (!rtf_emit_codepoint(parser, 0xfffdu, &active_bold, &active_italic,
                                &active_underline, &active_strike,
                                &active_font_size)) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (!rtf_close_paragraph(parser, &active_bold, &active_italic,
                             &active_underline, &active_strike,
                             &active_font_size) ||
        !qw_text_buffer_append(&parser->html, suffix, sizeof(suffix) - 1u))
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    return QW_STATUS_OK;
}


int qw_rtf_parse(const void *data,
                 size_t size,
                 QwTextBuffer *out_html,
                 QwRtfParseSummary *out_summary)
{
    QwRtfParser parser;
    int rc;
    if (!out_html || !out_summary) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_html, 0, sizeof(*out_html));
    memset(out_summary, 0, sizeof(*out_summary));
    if (!data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!qw_rtf_probe(data, size)) return QW_STATUS_ERR_FORMAT;
    if (size > QW_RTF_MAX_INPUT) return QW_STATUS_ERR_UNSUPPORTED;

    memset(&parser, 0, sizeof(parser));
    rc = rtf_parse_internal(&parser, (const unsigned char *)data, size);
    if (rc == QW_STATUS_OK) {
        *out_html = parser.html;
        parser.html.data = NULL;
        out_summary->saw_table = parser.saw_table;
        out_summary->saw_picture = parser.saw_picture;
        out_summary->rendered_picture = parser.rendered_picture;
        out_summary->omitted_picture = parser.omitted_picture;
        out_summary->saw_object = parser.saw_object;
        out_summary->saw_header_footer = parser.saw_header_footer;
        out_summary->saw_footnote = parser.saw_footnote;
    }
    free(parser.html.data);
    return rc;
}
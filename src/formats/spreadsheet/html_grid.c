#include "html_grid.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    HTML_GRID_MAX_INPUT_BYTES = 512 * 1024 * 1024,
    HTML_GRID_MAX_CELL_BYTES = 256 * 1024 * 1024,
    HTML_GRID_MAX_SHEET_NAME_BYTES = 4096,
    HTML_GRID_MAX_TABLES = 256
};

typedef struct HtmlGridBuffer {
    char *data;
    size_t size;
    size_t capacity;
    size_t limit;
} HtmlGridBuffer;

static int html_grid_buffer_append(HtmlGridBuffer *buffer,
                                   const char *data, size_t size)
{
    size_t required;
    size_t capacity;
    char *grown;
    if (!buffer || (!data && size) || buffer->size == SIZE_MAX ||
        size > SIZE_MAX - buffer->size - 1u) return 0;
    required = buffer->size + size + 1u;
    if (required > buffer->limit) return 0;
    if (required > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 4096u;
        while (capacity < required) {
            if (capacity > buffer->limit / 2u) {
                capacity = buffer->limit;
                break;
            }
            capacity *= 2u;
        }
        if (capacity < required) return 0;
        grown = (char *)realloc(buffer->data, capacity);
        if (!grown) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    if (size) memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

typedef enum HtmlGridTag {
    HTML_GRID_TAG_UNKNOWN = 0,
    HTML_GRID_TAG_COMMENT,
    HTML_GRID_TAG_DECLARATION,
    HTML_GRID_TAG_HTML,
    HTML_GRID_TAG_HEAD,
    HTML_GRID_TAG_BODY,
    HTML_GRID_TAG_META,
    HTML_GRID_TAG_STYLE,
    HTML_GRID_TAG_SECTION,
    HTML_GRID_TAG_H2,
    HTML_GRID_TAG_TABLE,
    HTML_GRID_TAG_TBODY,
    HTML_GRID_TAG_TR,
    HTML_GRID_TAG_TD,
    HTML_GRID_TAG_TH
} HtmlGridTag;

typedef struct HtmlGridState {
    const QwHtmlGridVisitor *visitor;
    const QwHtmlGridDocumentVisitor *document_visitor;
    void *context;
    HtmlGridBuffer cell_text;
    HtmlGridBuffer heading_text;
    size_t table_count;
    size_t row_count;
    size_t total_row_count;
    size_t column_count;
    int in_table;
    int in_tbody;
    int saw_tbody;
    int in_row;
    int in_cell;
    int cell_is_th;
    int cell_is_row_header;
    int in_h2;
} HtmlGridState;

static int html_grid_tag_name(const uint8_t *data, size_t size,
                              HtmlGridTag *out_tag)
{
#define HTML_GRID_TAG(name, value) \
    if (size == sizeof(name) - 1u && \
        memcmp(data, name, sizeof(name) - 1u) == 0) { \
        *out_tag = value; return 1; \
    }
    HTML_GRID_TAG("html", HTML_GRID_TAG_HTML)
    HTML_GRID_TAG("head", HTML_GRID_TAG_HEAD)
    HTML_GRID_TAG("body", HTML_GRID_TAG_BODY)
    HTML_GRID_TAG("meta", HTML_GRID_TAG_META)
    HTML_GRID_TAG("style", HTML_GRID_TAG_STYLE)
    HTML_GRID_TAG("section", HTML_GRID_TAG_SECTION)
    HTML_GRID_TAG("h2", HTML_GRID_TAG_H2)
    HTML_GRID_TAG("table", HTML_GRID_TAG_TABLE)
    HTML_GRID_TAG("tbody", HTML_GRID_TAG_TBODY)
    HTML_GRID_TAG("tr", HTML_GRID_TAG_TR)
    HTML_GRID_TAG("td", HTML_GRID_TAG_TD)
    HTML_GRID_TAG("th", HTML_GRID_TAG_TH)
#undef HTML_GRID_TAG
    return 0;
}

static int html_grid_parse_tag(const uint8_t *data, size_t size,
                               size_t *position, HtmlGridTag *out_tag,
                               int *out_closing, int *out_row_header)
{
    size_t start;
    size_t name_start;
    size_t name_size;
    size_t end;
    int quote = 0;
    int closing = 0;
    HtmlGridTag tag = HTML_GRID_TAG_UNKNOWN;

    if (!data || !position || !out_tag || !out_closing || !out_row_header ||
        *position >= size || data[*position] != '<') return QW_HTML_GRID_ERROR_FORMAT;
    start = *position;
    if (size - start >= 4u && memcmp(data + start, "<!--", 4u) == 0) {
        end = start + 4u;
        while (end + 2u < size && memcmp(data + end, "-->", 3u) != 0) ++end;
        if (end + 2u >= size) return QW_HTML_GRID_ERROR_FORMAT;
        *position = end + 3u;
        *out_tag = HTML_GRID_TAG_COMMENT;
        *out_closing = 0;
        *out_row_header = 0;
        return QW_HTML_GRID_OK;
    }
    if (size - start >= 2u && data[start + 1u] == '!') {
        end = start + 2u;
        while (end < size && data[end] != '>') ++end;
        if (end == size) return QW_HTML_GRID_ERROR_FORMAT;
        *position = end + 1u;
        *out_tag = HTML_GRID_TAG_DECLARATION;
        *out_closing = 0;
        *out_row_header = 0;
        return QW_HTML_GRID_OK;
    }

    name_start = start + 1u;
    if (name_start < size && data[name_start] == '/') {
        closing = 1;
        ++name_start;
    }
    while (name_start < size && isspace((unsigned char)data[name_start]))
        ++name_start;
    end = name_start;
    while (end < size && ((data[end] >= 'a' && data[end] <= 'z') ||
           (data[end] >= 'A' && data[end] <= 'Z') ||
           (data[end] >= '0' && data[end] <= '9') || data[end] == '-')) ++end;
    if (end == name_start || end - name_start >= 16u) return QW_HTML_GRID_ERROR_FORMAT;
    name_size = end - name_start;
    {
        uint8_t lowercase[16];
        size_t i;
        for (i = 0u; i < name_size; ++i)
            lowercase[i] = (uint8_t)tolower((unsigned char)data[name_start + i]);
        (void)html_grid_tag_name(lowercase, name_size, &tag);
    }
    end = name_start + name_size;
    while (end < size) {
        uint8_t ch = data[end++];
        if (quote) {
            if (ch == (uint8_t)quote) quote = 0;
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '>') {
            break;
        }
    }
    if (end > size || !end || data[end - 1u] != '>') return QW_HTML_GRID_ERROR_FORMAT;
    if (tag == HTML_GRID_TAG_UNKNOWN) return QW_HTML_GRID_ERROR_UNSUPPORTED;
    *out_row_header = 0;
    if (tag == HTML_GRID_TAG_TH && !closing) {
        size_t i;
        for (i = name_start + name_size; i + 11u <= end - 1u; ++i) {
            if (memcmp(data + i, "scope=\"row\"", 11u) == 0 ||
                memcmp(data + i, "scope='row'", 11u) == 0) {
                *out_row_header = 1;
                break;
            }
        }
    }
    *position = end;
    *out_tag = tag;
    *out_closing = closing;
    return QW_HTML_GRID_OK;
}

static int html_grid_append_codepoint(HtmlGridBuffer *text, uint32_t value)
{
    char bytes[4];
    size_t size;
    if (!value || value > 0x10ffffu ||
        (value >= 0xd800u && value <= 0xdfffu)) return 0;
    if (value < 0x80u) {
        bytes[0] = (char)value;
        size = 1u;
    } else if (value < 0x800u) {
        bytes[0] = (char)(0xc0u | (value >> 6));
        bytes[1] = (char)(0x80u | (value & 0x3fu));
        size = 2u;
    } else if (value < 0x10000u) {
        bytes[0] = (char)(0xe0u | (value >> 12));
        bytes[1] = (char)(0x80u | ((value >> 6) & 0x3fu));
        bytes[2] = (char)(0x80u | (value & 0x3fu));
        size = 3u;
    } else {
        bytes[0] = (char)(0xf0u | (value >> 18));
        bytes[1] = (char)(0x80u | ((value >> 12) & 0x3fu));
        bytes[2] = (char)(0x80u | ((value >> 6) & 0x3fu));
        bytes[3] = (char)(0x80u | (value & 0x3fu));
        size = 4u;
    }
    return html_grid_buffer_append(text, bytes, size);
}

static int html_grid_decode_entity(const uint8_t *data, size_t size,
                                   size_t *position, uint32_t *out_value)
{
    size_t start = *position + 1u;
    size_t end = start;
    uint32_t value = 0u;
    unsigned base = 10u;
    size_t i;
    if (start >= size) return 0;
    while (end < size && data[end] != ';' && end - start <= 12u) ++end;
    if (end >= size || data[end] != ';' || end == start) return 0;
    if (end - start == 3u && memcmp(data + start, "amp", 3u) == 0)
        value = '&';
    else if (end - start == 2u && memcmp(data + start, "lt", 2u) == 0)
        value = '<';
    else if (end - start == 2u && memcmp(data + start, "gt", 2u) == 0)
        value = '>';
    else if (end - start == 4u && memcmp(data + start, "quot", 4u) == 0)
        value = '"';
    else if (end - start == 4u && memcmp(data + start, "apos", 4u) == 0)
        value = '\'';
    else if (end - start == 4u && memcmp(data + start, "nbsp", 4u) == 0)
        value = 0xa0u;
    else if (data[start] == '#') {
        i = start + 1u;
        if (i < end && (data[i] == 'x' || data[i] == 'X')) {
            base = 16u;
            ++i;
        }
        if (i == end) return 0;
        for (; i < end; ++i) {
            unsigned digit;
            uint8_t ch = data[i];
            if (ch >= '0' && ch <= '9') digit = (unsigned)(ch - '0');
            else if (base == 16u && ch >= 'a' && ch <= 'f')
                digit = (unsigned)(ch - 'a') + 10u;
            else if (base == 16u && ch >= 'A' && ch <= 'F')
                digit = (unsigned)(ch - 'A') + 10u;
            else return 0;
            if (digit >= base || value > (0x10ffffu - digit) / base) return 0;
            value = value * base + digit;
        }
    } else {
        return 0;
    }
    *position = end + 1u;
    *out_value = value;
    return 1;
}

static int html_grid_append_text(HtmlGridBuffer *text, const uint8_t *data,
                                 size_t size)
{
    size_t i = 0u;
    size_t span = 0u;
    if (memchr(data, 0, size)) return QW_HTML_GRID_ERROR_FORMAT;
    while (i < size) {
        uint32_t codepoint;
        if (data[i] != '&') {
            ++i;
            continue;
        }
        if (i > span && !html_grid_buffer_append(text,
                (const char *)data + span, i - span)) return QW_HTML_GRID_ERROR_RESOURCE_LIMIT;
        if (!html_grid_decode_entity(data, size, &i, &codepoint) ||
            !html_grid_append_codepoint(text, codepoint))
            return QW_HTML_GRID_ERROR_FORMAT;
        span = i;
    }
    if (span < size && !html_grid_buffer_append(text,
            (const char *)data + span, size - span)) return QW_HTML_GRID_ERROR_RESOURCE_LIMIT;
    return QW_HTML_GRID_OK;
}

static int html_grid_text_is_space(const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        if (data[i] != ' ' && data[i] != '\t' &&
            data[i] != '\r' && data[i] != '\n') return 0;
    }
    return 1;
}

static const uint8_t *html_grid_find_style_end(const uint8_t *data,
                                              size_t size, size_t start)
{
    static const char end_tag[] = "</style>";
    size_t i;
    for (i = start; i + sizeof(end_tag) - 1u <= size; ++i) {
        size_t j;
        for (j = 0u; j < sizeof(end_tag) - 1u; ++j) {
            if ((uint8_t)tolower((unsigned char)data[i + j]) !=
                (uint8_t)end_tag[j]) break;
        }
        if (j == sizeof(end_tag) - 1u) return data + i;
    }
    return NULL;
}

static int html_grid_process_tag(HtmlGridState *state, HtmlGridTag tag,
                                 int closing, int row_header)
{
    if (tag == HTML_GRID_TAG_COMMENT || tag == HTML_GRID_TAG_DECLARATION)
        return state->in_table ? QW_HTML_GRID_ERROR_UNSUPPORTED : QW_HTML_GRID_OK;
    if (!state->in_table) {
        if (tag == HTML_GRID_TAG_TABLE && !closing) {
            const uint8_t *name = NULL;
            size_t name_size = 0u;
            int rc;
            if (state->in_h2 || ++state->table_count > HTML_GRID_MAX_TABLES ||
                (!state->document_visitor && state->table_count != 1u))
                return QW_HTML_GRID_ERROR_UNSUPPORTED;
            if (state->heading_text.data) {
                size_t start = 0u;
                size_t end = state->heading_text.size;
                while (start < end && (state->heading_text.data[start] == ' ' ||
                       state->heading_text.data[start] == '\t' ||
                       state->heading_text.data[start] == '\r' ||
                       state->heading_text.data[start] == '\n')) ++start;
                while (end > start && (state->heading_text.data[end - 1u] == ' ' ||
                       state->heading_text.data[end - 1u] == '\t' ||
                       state->heading_text.data[end - 1u] == '\r' ||
                       state->heading_text.data[end - 1u] == '\n')) --end;
                if (end > start) {
                    name = (const uint8_t *)state->heading_text.data + start;
                    name_size = end - start;
                }
            }
            if (state->document_visitor) {
                rc = state->document_visitor->begin_table(
                    state->context, state->table_count - 1u, name, name_size);
                if (rc != QW_HTML_GRID_OK) return rc;
            }
            state->heading_text.size = 0u;
            if (state->heading_text.data) state->heading_text.data[0] = '\0';
            state->in_table = 1;
            state->in_tbody = 0;
            state->saw_tbody = 0;
            state->row_count = 0u;
            return QW_HTML_GRID_OK;
        }
        if (tag == HTML_GRID_TAG_H2) {
            if (closing) {
                if (!state->in_h2) return QW_HTML_GRID_ERROR_FORMAT;
                state->in_h2 = 0;
            } else {
                if (state->in_h2) return QW_HTML_GRID_ERROR_FORMAT;
                state->in_h2 = 1;
                state->heading_text.size = 0u;
                if (state->heading_text.data)
                    state->heading_text.data[0] = '\0';
            }
            return QW_HTML_GRID_OK;
        }
        if (tag == HTML_GRID_TAG_HTML || tag == HTML_GRID_TAG_HEAD ||
            tag == HTML_GRID_TAG_BODY || tag == HTML_GRID_TAG_META ||
            tag == HTML_GRID_TAG_STYLE || tag == HTML_GRID_TAG_SECTION)
            return QW_HTML_GRID_OK;
        return QW_HTML_GRID_ERROR_FORMAT;
    }

    if (tag == HTML_GRID_TAG_TBODY) {
        if (closing) {
            if (!state->in_tbody || state->in_row) return QW_HTML_GRID_ERROR_FORMAT;
            state->in_tbody = 0;
        } else {
            if (state->saw_tbody || state->in_row) return QW_HTML_GRID_ERROR_FORMAT;
            state->saw_tbody = 1;
            state->in_tbody = 1;
        }
        return QW_HTML_GRID_OK;
    }
    if (tag == HTML_GRID_TAG_TR) {
        if (closing) {
            int rc;
            if (!state->in_row || state->in_cell) return QW_HTML_GRID_ERROR_FORMAT;
            rc = state->visitor->end_row(state->context, state->row_count);
            if (rc != QW_HTML_GRID_OK) return rc;
            ++state->row_count;
            ++state->total_row_count;
            state->in_row = 0;
        } else {
            int rc;
            if (state->in_row || (state->saw_tbody && !state->in_tbody))
                return QW_HTML_GRID_ERROR_FORMAT;
            state->in_row = 1;
            state->column_count = 0u;
            rc = state->visitor->begin_row(state->context, state->row_count);
            if (rc != QW_HTML_GRID_OK) return rc;
        }
        return QW_HTML_GRID_OK;
    }
    if (tag == HTML_GRID_TAG_TD || tag == HTML_GRID_TAG_TH) {
        if (closing) {
            int rc = QW_HTML_GRID_OK;
            if (!state->in_cell || (tag == HTML_GRID_TAG_TH) != state->cell_is_th)
                return QW_HTML_GRID_ERROR_FORMAT;
            if (state->cell_is_row_header) {
                if (state->column_count) return QW_HTML_GRID_ERROR_FORMAT;
            } else {
                rc = state->visitor->cell(state->context, state->row_count,
                    state->column_count, (const uint8_t *)state->cell_text.data,
                    state->cell_text.size);
                if (rc != QW_HTML_GRID_OK) return rc;
                ++state->column_count;
            }
            state->cell_text.size = 0u;
            if (state->cell_text.data) state->cell_text.data[0] = '\0';
            state->in_cell = 0;
            state->cell_is_th = 0;
            state->cell_is_row_header = 0;
        } else {
            if (!state->in_row || state->in_cell ||
                (row_header && (tag != HTML_GRID_TAG_TH || state->column_count)))
                return QW_HTML_GRID_ERROR_FORMAT;
            state->in_cell = 1;
            state->cell_is_th = tag == HTML_GRID_TAG_TH;
            state->cell_is_row_header = row_header;
            state->cell_text.size = 0u;
            if (state->cell_text.data) state->cell_text.data[0] = '\0';
        }
        return QW_HTML_GRID_OK;
    }
    if (tag == HTML_GRID_TAG_TABLE && closing) {
        int rc;
        if (state->in_row || state->in_tbody || !state->row_count)
            return QW_HTML_GRID_ERROR_FORMAT;
        if (state->document_visitor) {
            rc = state->document_visitor->end_table(state->context,
                state->table_count - 1u);
            if (rc != QW_HTML_GRID_OK) return rc;
        }
        state->in_table = 0;
        return QW_HTML_GRID_OK;
    }
    return QW_HTML_GRID_ERROR_FORMAT;
}

static int html_grid_visit(const uint8_t *html, size_t html_size,
                           const QwHtmlGridVisitor *visitor,
                           const QwHtmlGridDocumentVisitor *document_visitor,
                           void *context)
{
    HtmlGridState state;
    size_t position = 0u;
    int rc = QW_HTML_GRID_ERROR_UNSUPPORTED;
    if (!html || !html_size || !visitor || !visitor->begin_row ||
        !visitor->cell || !visitor->end_row) return QW_HTML_GRID_ERROR_INVALID_ARGUMENT;
    if (html_size > HTML_GRID_MAX_INPUT_BYTES) return QW_HTML_GRID_ERROR_RESOURCE_LIMIT;
    memset(&state, 0, sizeof(state));
    state.visitor = visitor;
    state.document_visitor = document_visitor;
    state.context = context;
    state.cell_text.limit = (size_t)HTML_GRID_MAX_CELL_BYTES + 1u;
    state.heading_text.limit = (size_t)HTML_GRID_MAX_SHEET_NAME_BYTES + 1u;

    while (position < html_size) {
        if (html[position] != '<') {
            size_t start = position;
            while (position < html_size && html[position] != '<') ++position;
            if (state.in_cell) {
                rc = html_grid_append_text(&state.cell_text, html + start,
                                           position - start);
                if (rc != QW_HTML_GRID_OK) goto done;
            } else if (!state.in_table && state.in_h2) {
                rc = html_grid_append_text(&state.heading_text, html + start,
                                           position - start);
                if (rc != QW_HTML_GRID_OK) goto done;
            } else if (html_grid_text_is_space(html + start, position - start)) {
                /* Worksheet headings and whitespace do not become cells. */
            } else {
                rc = QW_HTML_GRID_ERROR_UNSUPPORTED;
                goto done;
            }
            continue;
        }
        {
            HtmlGridTag tag;
            int closing;
            int row_header;
            rc = html_grid_parse_tag(html, html_size, &position, &tag,
                                     &closing, &row_header);
            if (rc != QW_HTML_GRID_OK) goto done;
            if (tag == HTML_GRID_TAG_STYLE && !closing && !state.in_table) {
                const uint8_t *end = html_grid_find_style_end(
                    html, html_size, position);
                if (!end) {
                    rc = QW_HTML_GRID_ERROR_FORMAT;
                    goto done;
                }
                position = (size_t)(end - html);
                continue;
            }
            rc = html_grid_process_tag(&state, tag, closing, row_header);
            if (rc != QW_HTML_GRID_OK) goto done;
        }
    }
    if ((!document_visitor && state.table_count != 1u) ||
        (document_visitor && !state.table_count) || state.in_table ||
        state.in_row || state.in_cell || state.in_h2 ||
        !state.total_row_count) {
        rc = QW_HTML_GRID_ERROR_UNSUPPORTED;
        goto done;
    }
    rc = QW_HTML_GRID_OK;

done:
    free(state.cell_text.data);
    free(state.heading_text.data);
    return rc;
}

int qw_html_grid_visit_single_table(const uint8_t *html, size_t html_size,
                                    const QwHtmlGridVisitor *visitor,
                                    void *context)
{
    return html_grid_visit(html, html_size, visitor, NULL, context);
}

int qw_html_grid_visit_tables(const uint8_t *html, size_t html_size,
                              const QwHtmlGridDocumentVisitor *visitor,
                              void *context)
{
    if (!visitor || !visitor->begin_table || !visitor->end_table)
        return QW_HTML_GRID_ERROR_INVALID_ARGUMENT;
    return html_grid_visit(html, html_size, &visitor->grid, visitor, context);
}

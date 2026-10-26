#include "markdown_parser.h"

#include "format_registry.h"

#include <stdio.h>
#include <string.h>

enum {
    QW_MARKDOWN_PARSE_MAX_INPUT = 256 * 1024 * 1024,
    QW_MARKDOWN_PARSE_MAX_DEPTH = 32,
    QW_MARKDOWN_PROBE_LIMIT = 65536
};

typedef struct QwMarkdownLine {
    const uint8_t *data;
    size_t size;
} QwMarkdownLine;

typedef struct QwMarkdownFence {
    uint8_t marker;
    size_t marker_count;
} QwMarkdownFence;

typedef struct QwMarkdownListMarker {
    int ordered;
    uint64_t start_number;
    size_t content_offset;
} QwMarkdownListMarker;

static int markdown_append(QwTextBuffer *buffer, const char *data, size_t size)
{
    if (!buffer || (!data && size)) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (buffer->size == SIZE_MAX || size > SIZE_MAX - buffer->size - 1u ||
        (buffer->limit && size + buffer->size + 1u > buffer->limit))
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    return qw_text_buffer_append(buffer, data, size)
               ? QW_STATUS_OK : QW_STATUS_ERR_OUT_OF_MEMORY;
}

static int markdown_append_literal(QwTextBuffer *buffer, const char *text)
{
    return markdown_append(buffer, text, strlen(text));
}

static int markdown_read_line(const uint8_t *data, size_t size,
                              size_t *position, QwMarkdownLine *out_line)
{
    size_t start;
    size_t end;
    if (!data || !position || !out_line || *position > size)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (*position == size) return 0;
    start = *position;
    end = start;
    while (end < size && data[end] != '\n' && data[end] != '\r') ++end;
    out_line->data = data + start;
    out_line->size = end - start;
    if (end < size && data[end] == '\r') ++end;
    if (end < size && data[end] == '\n') ++end;
    *position = end;
    return 1;
}

static size_t markdown_leading_spaces(const QwMarkdownLine *line)
{
    size_t count = 0u;
    while (count < line->size && count < 4u && line->data[count] == ' ')
        ++count;
    return count;
}

static int markdown_line_blank(const QwMarkdownLine *line)
{
    for (size_t i = 0u; i < line->size; ++i)
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    return 1;
}

static int markdown_heading(const QwMarkdownLine *line,
                            size_t *out_level, size_t *out_content)
{
    size_t offset = markdown_leading_spaces(line);
    size_t hashes = 0u;
    if (offset > 3u || offset >= line->size ||
        line->data[offset] != '#') return 0;
    while (offset + hashes < line->size &&
           line->data[offset + hashes] == '#' && hashes < 7u) ++hashes;
    if (!hashes || hashes > 6u ||
        (offset + hashes < line->size &&
         line->data[offset + hashes] != ' ' &&
         line->data[offset + hashes] != '\t')) return 0;
    offset += hashes;
    while (offset < line->size &&
           (line->data[offset] == ' ' || line->data[offset] == '\t'))
        ++offset;
    *out_level = hashes;
    *out_content = offset;
    return 1;
}

static int markdown_horizontal_rule(const QwMarkdownLine *line)
{
    size_t offset = markdown_leading_spaces(line);
    uint8_t marker;
    size_t count = 0u;
    if (offset > 3u) return 0;
    while (offset < line->size &&
           (line->data[offset] == ' ' || line->data[offset] == '\t'))
        ++offset;
    if (offset >= line->size ||
        (line->data[offset] != '-' && line->data[offset] != '*' &&
         line->data[offset] != '_')) return 0;
    marker = line->data[offset];
    for (; offset < line->size; ++offset) {
        if (line->data[offset] == marker) ++count;
        else if (line->data[offset] != ' ' && line->data[offset] != '\t')
            return 0;
    }
    return count >= 3u;
}

static int markdown_fence(const QwMarkdownLine *line,
                          QwMarkdownFence *out_fence)
{
    size_t offset = markdown_leading_spaces(line);
    size_t count = 0u;
    uint8_t marker;
    if (offset > 3u || offset >= line->size ||
        (line->data[offset] != (uint8_t)0x60u &&
         line->data[offset] != '~')) return 0;
    marker = line->data[offset];
    while (offset + count < line->size &&
           line->data[offset + count] == marker) ++count;
    if (count < 3u) return 0;
    out_fence->marker = marker;
    out_fence->marker_count = count;
    return 1;
}

static int markdown_fence_close(const QwMarkdownLine *line,
                                const QwMarkdownFence *fence)
{
    size_t offset = markdown_leading_spaces(line);
    size_t count = 0u;
    if (offset > 3u || offset >= line->size ||
        line->data[offset] != fence->marker) return 0;
    while (offset + count < line->size &&
           line->data[offset + count] == fence->marker) ++count;
    if (count < fence->marker_count) return 0;
    offset += count;
    while (offset < line->size &&
           (line->data[offset] == ' ' || line->data[offset] == '\t'))
        ++offset;
    return offset == line->size;
}

static int markdown_blockquote(const QwMarkdownLine *line,
                               size_t *out_content)
{
    size_t offset = markdown_leading_spaces(line);
    if (offset > 3u || offset >= line->size || line->data[offset] != '>')
        return 0;
    ++offset;
    if (offset < line->size &&
        (line->data[offset] == ' ' || line->data[offset] == '\t'))
        ++offset;
    *out_content = offset;
    return 1;
}

static int markdown_list_marker(const QwMarkdownLine *line,
                                QwMarkdownListMarker *out_marker)
{
    size_t offset = markdown_leading_spaces(line);
    size_t digits = 0u;
    uint64_t number = 0u;
    if (offset > 3u || offset >= line->size) return 0;
    if ((line->data[offset] == '-' || line->data[offset] == '+' ||
         line->data[offset] == '*') &&
        offset + 1u < line->size &&
        (line->data[offset + 1u] == ' ' ||
         line->data[offset + 1u] == '\t')) {
        out_marker->ordered = 0;
        out_marker->start_number = 1u;
        offset += 2u;
        while (offset < line->size &&
               (line->data[offset] == ' ' || line->data[offset] == '\t'))
            ++offset;
        out_marker->content_offset = offset;
        return 1;
    }
    while (offset + digits < line->size && digits < 9u &&
           line->data[offset + digits] >= '0' &&
           line->data[offset + digits] <= '9') {
        number = number * 10u +
                 (uint64_t)(line->data[offset + digits] - '0');
        ++digits;
    }
    if (!digits || offset + digits + 1u >= line->size ||
        (line->data[offset + digits] != '.' &&
         line->data[offset + digits] != ')') ||
        (line->data[offset + digits + 1u] != ' ' &&
         line->data[offset + digits + 1u] != '\t')) return 0;
    offset += digits + 2u;
    while (offset < line->size &&
           (line->data[offset] == ' ' || line->data[offset] == '\t'))
        ++offset;
    out_marker->ordered = 1;
    out_marker->start_number = number ? number : 1u;
    out_marker->content_offset = offset;
    return 1;
}

static int markdown_line_starts_block(const QwMarkdownLine *line)
{
    size_t level, content;
    QwMarkdownFence fence;
    QwMarkdownListMarker marker;
    return markdown_heading(line, &level, &content) ||
           markdown_horizontal_rule(line) ||
           markdown_fence(line, &fence) ||
           markdown_blockquote(line, &content) ||
           markdown_list_marker(line, &marker);
}

static int markdown_utf8_valid(const uint8_t *data, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        uint8_t a = data[i];
        size_t length;
        if (a < 0x80u) {
            if (a == 0u || a < 0x09u ||
                (a > 0x0du && a < 0x20u)) return 0;
            ++i;
            continue;
        }
        if (a >= 0xc2u && a <= 0xdfu) length = 2u;
        else if (a >= 0xe0u && a <= 0xefu) length = 3u;
        else if (a >= 0xf0u && a <= 0xf4u) length = 4u;
        else return 0;
        if (length > size - i) return 0;
        if ((data[i + 1u] & 0xc0u) != 0x80u) return 0;
        if (length >= 3u && (data[i + 2u] & 0xc0u) != 0x80u) return 0;
        if (length == 4u && (data[i + 3u] & 0xc0u) != 0x80u) return 0;
        if (a == 0xe0u && data[i + 1u] < 0xa0u) return 0;
        if (a == 0xedu && data[i + 1u] >= 0xa0u) return 0;
        if (a == 0xf0u && data[i + 1u] < 0x90u) return 0;
        if (a == 0xf4u && data[i + 1u] >= 0x90u) return 0;
        i += length;
    }
    return 1;
}

static int markdown_entity_length(const uint8_t *data, size_t size,
                                  size_t *out_length)
{
    size_t i = 1u;
    if (!data || size < 3u || data[0] != '&') return 0;
    if (data[i] == '#') {
        ++i;
        if (i < size && (data[i] == 'x' || data[i] == 'X')) ++i;
        if (i >= size || data[i] == ';') return 0;
    }
    for (; i < size && i <= 32u; ++i) {
        uint8_t byte = data[i];
        if (byte == ';') {
            if (i == 1u) return 0;
            *out_length = i + 1u;
            return 1;
        }
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= 'A' && byte <= 'Z') ||
              (byte >= '0' && byte <= '9'))) return 0;
    }
    return 0;
}

static int markdown_escape_html(QwTextBuffer *buffer,
                                const uint8_t *data, size_t size,
                                int preserve_entities)
{
    size_t i = 0u;
    while (i < size) {
        const char *replacement = NULL;
        size_t replacement_size = 0u;
        size_t entity_size = 0u;
        if (preserve_entities && data[i] == '&' &&
            markdown_entity_length(data + i, size - i, &entity_size)) {
            int rc = markdown_append(buffer, (const char *)data + i,
                                     entity_size);
            if (rc != QW_STATUS_OK) return rc;
            i += entity_size;
            continue;
        }
        switch (data[i]) {
        case '&': replacement = "&amp;"; replacement_size = 5u; break;
        case '<': replacement = "&lt;"; replacement_size = 4u; break;
        case '>': replacement = "&gt;"; replacement_size = 4u; break;
        case '"': replacement = "&quot;"; replacement_size = 6u; break;
        case '\'': replacement = "&#39;"; replacement_size = 5u; break;
        default: break;
        }
        if (replacement) {
            int rc = markdown_append(buffer, replacement, replacement_size);
            if (rc != QW_STATUS_OK) return rc;
            ++i;
        } else {
            size_t chunk_start = i++;
            while (i < size && data[i] != '&' && data[i] != '<' &&
                   data[i] != '>' && data[i] != '"' && data[i] != '\'')
                ++i;
            {
                int rc = markdown_append(buffer,
                    (const char *)data + chunk_start, i - chunk_start);
                if (rc != QW_STATUS_OK) return rc;
            }
        }
    }
    return QW_STATUS_OK;
}

static int markdown_url_safe(const uint8_t *url, size_t size)
{
    size_t scheme_end = SIZE_MAX;
    if (!url && size) return 0;
    for (size_t i = 0u; i < size; ++i) {
        uint8_t byte = url[i];
        if (byte <= 0x20u || byte == 0x7fu || byte == '"' ||
            byte == '\'' || byte == '<' || byte == '>') return 0;
    }
    for (size_t i = 0u; i < size; ++i) {
        uint8_t byte = url[i];
        if (byte == '/' || byte == '?' || byte == '#') break;
        if (byte == ':') {
            scheme_end = i;
            break;
        }
    }
    if (scheme_end == SIZE_MAX) return 1;
    if (scheme_end == 4u &&
        (url[0] == 'h' || url[0] == 'H') &&
        (url[1] == 't' || url[1] == 'T') &&
        (url[2] == 't' || url[2] == 'T') &&
        (url[3] == 'p' || url[3] == 'P')) return 1;
    if (scheme_end == 5u &&
        (url[0] == 'h' || url[0] == 'H') &&
        (url[1] == 't' || url[1] == 'T') &&
        (url[2] == 't' || url[2] == 'T') &&
        (url[3] == 'p' || url[3] == 'P') &&
        (url[4] == 's' || url[4] == 'S')) return 1;
    if (scheme_end == 6u &&
        (url[0] == 'm' || url[0] == 'M') &&
        (url[1] == 'a' || url[1] == 'A') &&
        (url[2] == 'i' || url[2] == 'I') &&
        (url[3] == 'l' || url[3] == 'L') &&
        (url[4] == 't' || url[4] == 'T') &&
        (url[5] == 'o' || url[5] == 'O')) return 1;
    if (scheme_end == 3u &&
        (url[0] == 't' || url[0] == 'T') &&
        (url[1] == 'e' || url[1] == 'E') &&
        (url[2] == 'l' || url[2] == 'L')) return 1;
    return 0;
}

static size_t markdown_find_unescaped(const uint8_t *data, size_t size,
                                      size_t start, uint8_t marker)
{
    size_t i = start;
    while (i < size) {
        if (data[i] == '\\' && i + 1u < size) {
            i += 2u;
            continue;
        }
        if (data[i] == marker) return i;
        ++i;
    }
    return SIZE_MAX;
}

static int markdown_link_parts(const uint8_t *data, size_t size,
                               size_t start, int image,
                               size_t *out_label_start,
                               size_t *out_label_size,
                               size_t *out_url_start,
                               size_t *out_url_size,
                               size_t *out_end)
{
    size_t label_start = start + (image ? 2u : 1u);
    size_t close_label;
    size_t position;
    size_t url_start;
    size_t url_end;
    size_t nesting = 0u;
    if (start >= size || (image && start + 1u >= size) ||
        data[start] != (image ? '!' : '[') ||
        data[image ? start + 1u : start] != '[') return 0;
    close_label = markdown_find_unescaped(data, size, label_start, ']');
    if (close_label == SIZE_MAX || close_label + 1u >= size ||
        data[close_label + 1u] != '(') return 0;
    position = close_label + 2u;
    while (position < size &&
           (data[position] == ' ' || data[position] == '\t'))
        ++position;
    if (position < size && data[position] == '<') {
        url_start = ++position;
        while (position < size && data[position] != '>') {
            if (data[position] == '\\' && position + 1u < size) position += 2u;
            else ++position;
        }
        if (position >= size) return 0;
        url_end = position;
        ++position;
    } else {
        url_start = position;
        while (position < size) {
            uint8_t byte = data[position];
            if (byte == '\\' && position + 1u < size) {
                position += 2u;
                continue;
            }
            if (byte == '(') {
                ++nesting;
                if (nesting > 8u) return 0;
                ++position;
                continue;
            }
            if (byte == ')') {
                if (!nesting) break;
                --nesting;
                ++position;
                continue;
            }
            if ((byte == ' ' || byte == '\t' || byte == '\r' ||
                 byte == '\n') && !nesting) break;
            ++position;
        }
        url_end = position;
    }
    while (position < size &&
           (data[position] == ' ' || data[position] == '\t' ||
            data[position] == '\r' || data[position] == '\n'))
        ++position;
    if (position < size && data[position] != ')') {
        uint8_t quote = data[position] == '\'' || data[position] == '"'
                            ? data[position] : 0u;
        if (quote) ++position;
        while (position < size &&
               (quote ? data[position] != quote : data[position] != ')')) {
            if (data[position] == '\\' && position + 1u < size)
                position += 2u;
            else
                ++position;
        }
        if (quote) {
            if (position >= size) return 0;
            ++position;
        }
        while (position < size &&
               (data[position] == ' ' || data[position] == '\t' ||
                data[position] == '\r' || data[position] == '\n'))
            ++position;
    }
    if (position >= size || data[position] != ')') return 0;
    *out_label_start = label_start;
    *out_label_size = close_label - label_start;
    *out_url_start = url_start;
    *out_url_size = url_end - url_start;
    *out_end = position + 1u;
    return 1;
}

static int markdown_inline(const uint8_t *data, size_t size,
                           QwTextBuffer *html, size_t *removed_images,
                           unsigned depth);

static int markdown_inline_delimiter(const uint8_t *data, size_t size,
                                     size_t start, size_t *out_width,
                                     const char **out_open,
                                     const char **out_close,
                                     size_t *out_close_at)
{
    uint8_t marker = data[start];
    size_t width = 1u;
    size_t close_at;
    if ((marker != '*' && marker != '_') || start + 1u >= size ||
        data[start + 1u] == ' ' || data[start + 1u] == '\t')
        return 0;
    if (start + 2u < size && data[start + 1u] == marker &&
        data[start + 2u] == marker) width = 3u;
    else if (data[start + 1u] == marker) width = 2u;
    if (start && marker == '_' &&
        ((data[start - 1u] >= 'a' && data[start - 1u] <= 'z') ||
         (data[start - 1u] >= 'A' && data[start - 1u] <= 'Z') ||
         (data[start - 1u] >= '0' && data[start - 1u] <= '9')))
        return 0;
    close_at = start + width;
    while (close_at < size) {
        size_t run = 1u;
        close_at = markdown_find_unescaped(data, size, close_at, marker);
        if (close_at == SIZE_MAX) return 0;
        while (close_at + run < size && data[close_at + run] == marker) ++run;
        if (run >= width && close_at > start + width &&
            data[close_at - 1u] != ' ' && data[close_at - 1u] != '\t')
            break;
        close_at += run;
    }
    if (close_at == SIZE_MAX || close_at + width > size) return 0;
    *out_open = width == 1u ? "<em>" :
                (width == 2u ? "<strong>" : "<em><strong>");
    *out_close = width == 1u ? "</em>" :
                 (width == 2u ? "</strong>" : "</strong></em>");
    *out_width = width;
    *out_close_at = close_at;
    return 1;
}

static int markdown_inline(const uint8_t *data, size_t size,
                           QwTextBuffer *html, size_t *removed_images,
                           unsigned depth)
{
    size_t i = 0u;
    if ((!data && size) || !html || !removed_images)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (depth > QW_MARKDOWN_PARSE_MAX_DEPTH) return QW_STATUS_ERR_UNSUPPORTED;
    while (i < size) {
        if (data[i] == '\\' && i + 1u < size &&
            data[i + 1u] >= 0x21u && data[i + 1u] <= 0x7eu) {
            int rc = markdown_escape_html(html, data + i + 1u, 1u, 1);
            if (rc != QW_STATUS_OK) return rc;
            i += 2u;
            continue;
        }
        if (data[i] == '!' || data[i] == '[') {
            int image = data[i] == '!';
            size_t label_start, label_size, url_start, url_size, token_end;
            if (markdown_link_parts(data, size, i, image,
                                    &label_start, &label_size,
                                    &url_start, &url_size, &token_end)) {
                int safe = markdown_url_safe(data + url_start, url_size);
                int rc;
                if (image) {
                    ++*removed_images;
                } else if (safe) {
                    rc = markdown_append_literal(html, "<a href=\"");
                    if (rc != QW_STATUS_OK) return rc;
                    rc = markdown_escape_html(html, data + url_start,
                                              url_size, 0);
                    if (rc != QW_STATUS_OK) return rc;
                    rc = markdown_append_literal(html, "\">");
                    if (rc != QW_STATUS_OK) return rc;
                }
                rc = markdown_inline(data + label_start, label_size, html,
                                     removed_images, depth + 1u);
                if (rc != QW_STATUS_OK) return rc;
                if (!image && safe) {
                    rc = markdown_append_literal(html, "</a>");
                    if (rc != QW_STATUS_OK) return rc;
                }
                i = token_end;
                continue;
            }
        }
        if (data[i] == (uint8_t)0x60u) {
            size_t run = 1u;
            size_t close;
            while (i + run < size && data[i + run] == (uint8_t)0x60u) ++run;
            close = i + run;
            while (close < size) {
                close = markdown_find_unescaped(data, size, close,
                                                (uint8_t)0x60u);
                if (close == SIZE_MAX) break;
                {
                    size_t close_run = 1u;
                    while (close + close_run < size &&
                           data[close + close_run] == (uint8_t)0x60u)
                        ++close_run;
                    if (close_run == run) break;
                    close += close_run;
                }
            }
            if (close != SIZE_MAX) {
                size_t content_start = i + run;
                size_t content_size = close - content_start;
                int rc = markdown_append_literal(html, "<code>");
                if (rc != QW_STATUS_OK) return rc;
                for (size_t k = 0u; k < content_size; ++k) {
                    uint8_t byte = data[content_start + k];
                    if (byte == '\n' || byte == '\r')
                        rc = markdown_append(html, " ", 1u);
                    else
                        rc = markdown_escape_html(html,
                            data + content_start + k, 1u, 0);
                    if (rc != QW_STATUS_OK) return rc;
                }
                rc = markdown_append_literal(html, "</code>");
                if (rc != QW_STATUS_OK) return rc;
                i = close + run;
                continue;
            }
        }
        if ((data[i] == '*' || data[i] == '_') && depth < 16u) {
            size_t width, close_at;
            const char *open;
            const char *close;
            if (markdown_inline_delimiter(data, size, i, &width,
                                          &open, &close, &close_at)) {
                int rc = markdown_append_literal(html, open);
                if (rc != QW_STATUS_OK) return rc;
                rc = markdown_inline(data + i + width,
                                     close_at - i - width, html,
                                     removed_images, depth + 1u);
                if (rc != QW_STATUS_OK) return rc;
                rc = markdown_append_literal(html, close);
                if (rc != QW_STATUS_OK) return rc;
                i = close_at + width;
                continue;
            }
        }
        if (data[i] == ' ' || data[i] == '\t') {
            size_t run = 1u;
            while (i + run < size &&
                   (data[i + run] == ' ' || data[i + run] == '\t')) ++run;
            if (run >= 2u && i + run < size &&
                (data[i + run] == '\n' || data[i + run] == '\r')) {
                int rc = markdown_append_literal(html, "<br />\n");
                if (rc != QW_STATUS_OK) return rc;
                i += run;
                if (data[i] == '\r') ++i;
                if (i < size && data[i] == '\n') ++i;
                continue;
            }
        }
        if (data[i] == '\n' || data[i] == '\r') {
            int rc = markdown_append(html, " ", 1u);
            if (rc != QW_STATUS_OK) return rc;
            if (data[i] == '\r') ++i;
            if (i < size && data[i] == '\n') ++i;
            continue;
        }
        {
            size_t entity_size;
            if (data[i] == '&' &&
                markdown_entity_length(data + i, size - i, &entity_size)) {
                int rc = markdown_append(html, (const char *)data + i,
                                         entity_size);
                if (rc != QW_STATUS_OK) return rc;
                i += entity_size;
                continue;
            }
        }
        {
            size_t start = i++;
            while (i < size && data[i] != '\\' && data[i] != '!' &&
                   data[i] != '[' && data[i] != (uint8_t)0x60u &&
                   data[i] != '*' && data[i] != '_' && data[i] != '&' &&
                   data[i] != '<' && data[i] != '>' && data[i] != '"' &&
                   data[i] != '\'' && data[i] != '\n' && data[i] != '\r' &&
                   data[i] != ' ' && data[i] != '\t')
                ++i;
            {
                int rc = markdown_escape_html(html, data + start,
                                              i - start, 1);
                if (rc != QW_STATUS_OK) return rc;
            }
        }
    }
    return QW_STATUS_OK;
}

static int markdown_render_fence(const uint8_t *data, size_t size,
                                 size_t *position,
                                 const QwMarkdownFence *fence,
                                 QwTextBuffer *html)
{
    QwMarkdownLine line;
    int rc = markdown_append_literal(html, "<pre><code>");
    if (rc != QW_STATUS_OK) return rc;
    while (markdown_read_line(data, size, position, &line) > 0) {
        if (markdown_fence_close(&line, fence)) break;
        rc = markdown_escape_html(html, line.data, line.size, 0);
        if (rc != QW_STATUS_OK) return rc;
        rc = markdown_append(html, "\n", 1u);
        if (rc != QW_STATUS_OK) return rc;
    }
    return markdown_append_literal(html, "</code></pre>\n");
}

static int markdown_render_heading(const QwMarkdownLine *line,
                                   size_t level, size_t content,
                                   QwTextBuffer *html,
                                   size_t *removed_images)
{
    char open[8];
    char close[9];
    size_t end = line->size;
    size_t hashes = 0u;
    if (content < end) {
        size_t cursor = end;
        while (cursor > content &&
               (line->data[cursor - 1u] == ' ' ||
                line->data[cursor - 1u] == '\t')) --cursor;
        while (cursor > content && line->data[cursor - 1u] == '#') {
            ++hashes;
            --cursor;
        }
        if (hashes && cursor > content &&
            (line->data[cursor - 1u] == ' ' ||
             line->data[cursor - 1u] == '\t')) {
            end = cursor - 1u;
            while (end > content &&
                   (line->data[end - 1u] == ' ' ||
                    line->data[end - 1u] == '\t')) --end;
        } else {
            end = line->size;
        }
    }
    (void)snprintf(open, sizeof(open), "<h%zu>", level);
    (void)snprintf(close, sizeof(close), "</h%zu>\n", level);
    {
        int rc = markdown_append_literal(html, open);
        if (rc != QW_STATUS_OK) return rc;
        rc = markdown_inline(line->data + content, end - content, html,
                             removed_images, 0u);
        if (rc != QW_STATUS_OK) return rc;
        return markdown_append_literal(html, close);
    }
}

static int markdown_render_blocks(const uint8_t *data, size_t size,
                                  QwTextBuffer *html,
                                  size_t *removed_images,
                                  unsigned depth);

static int markdown_render_blockquote(const uint8_t *data, size_t size,
                                      size_t *position,
                                      const QwMarkdownLine *first,
                                      QwTextBuffer *html,
                                      size_t *removed_images,
                                      unsigned depth)
{
    QwTextBuffer contents = {0};
    QwMarkdownLine line = *first;
    size_t content_offset;
    int rc;
    contents.limit = QW_MARKDOWN_PARSE_MAX_INPUT + 1u;
    rc = markdown_append_literal(html, "<blockquote>\n");
    if (rc != QW_STATUS_OK) goto cleanup;
    if (!markdown_blockquote(&line, &content_offset)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    rc = markdown_append(&contents,
                         (const char *)line.data + content_offset,
                         line.size - content_offset);
    if (rc != QW_STATUS_OK) goto cleanup;
    while (*position < size) {
        size_t before = *position;
        size_t next_position = before;
        if (markdown_read_line(data, size, &next_position, &line) <= 0)
            break;
        if (!markdown_blockquote(&line, &content_offset)) break;
        rc = markdown_append(&contents, "\n", 1u);
        if (rc == QW_STATUS_OK)
            rc = markdown_append(&contents,
                                 (const char *)line.data + content_offset,
                                 line.size - content_offset);
        if (rc != QW_STATUS_OK) goto cleanup;
        *position = next_position;
    }
    rc = markdown_render_blocks((const uint8_t *)contents.data, contents.size,
                                html, removed_images, depth + 1u);
    if (rc == QW_STATUS_OK)
        rc = markdown_append_literal(html, "</blockquote>\n");

cleanup:
    free(contents.data);
    return rc;
}

static int markdown_render_list(const uint8_t *data, size_t size,
                                size_t *position,
                                const QwMarkdownLine *first_line,
                                QwTextBuffer *html,
                                size_t *removed_images)
{
    QwMarkdownListMarker first;
    QwMarkdownListMarker marker;
    QwMarkdownLine line = *first_line;
    int rc;
    if (!markdown_list_marker(&line, &first)) return QW_STATUS_ERR_FORMAT;
    if (first.ordered) {
        if (first.start_number != 1u) {
            char start_attribute[48];
            (void)snprintf(start_attribute, sizeof(start_attribute),
                           "<ol start=\"%llu\">\n",
                           (unsigned long long)first.start_number);
            rc = markdown_append_literal(html, start_attribute);
        } else {
            rc = markdown_append_literal(html, "<ol>\n");
        }
    } else {
        rc = markdown_append_literal(html, "<ul>\n");
    }
    if (rc != QW_STATUS_OK) return rc;
    for (;;) {
        rc = markdown_append_literal(html, "<li>");
        if (rc != QW_STATUS_OK) return rc;
        rc = markdown_inline(line.data + first.content_offset,
                             line.size - first.content_offset,
                             html, removed_images, 0u);
        if (rc != QW_STATUS_OK) return rc;
        for (;;) {
            size_t before = *position;
            size_t next_position = before;
            if (markdown_read_line(data, size, &next_position, &line) <= 0)
                goto finish_item;
            if (markdown_line_blank(&line)) {
                *position = before;
                goto finish_item;
            }
            if (markdown_list_marker(&line, &marker) &&
                marker.ordered == first.ordered) {
                *position = next_position;
                rc = markdown_append_literal(html, "</li>\n<li>");
                if (rc != QW_STATUS_OK) return rc;
                rc = markdown_inline(line.data + marker.content_offset,
                                     line.size - marker.content_offset,
                                     html, removed_images, 0u);
                if (rc != QW_STATUS_OK) return rc;
                continue;
            }
            {
                size_t indent = markdown_leading_spaces(&line);
                if (indent >= 2u && indent <= 4u) {
                    *position = next_position;
                    rc = markdown_append(html, " ", 1u);
                    if (rc == QW_STATUS_OK)
                        rc = markdown_inline(line.data + indent,
                                             line.size - indent,
                                             html, removed_images, 0u);
                    if (rc != QW_STATUS_OK) return rc;
                    continue;
                }
            }
            *position = before;
            goto finish_item;
        }
finish_item:
        rc = markdown_append_literal(html, "</li>\n");
        if (rc != QW_STATUS_OK) return rc;
        if (*position >= size) break;
        {
            size_t before = *position;
            size_t next_position = before;
            if (markdown_read_line(data, size, &next_position, &line) <= 0)
                break;
            if (!markdown_list_marker(&line, &marker) ||
                marker.ordered != first.ordered) {
                *position = before;
                break;
            }
            first = marker;
            *position = next_position;
        }
    }
    return markdown_append_literal(html, first.ordered ? "</ol>\n" : "</ul>\n");
}

static int markdown_render_blocks(const uint8_t *data, size_t size,
                                  QwTextBuffer *html,
                                  size_t *removed_images,
                                  unsigned depth)
{
    size_t position = 0u;
    QwMarkdownLine line;
    if (depth > QW_MARKDOWN_PARSE_MAX_DEPTH) return QW_STATUS_ERR_UNSUPPORTED;
    while (markdown_read_line(data, size, &position, &line) > 0) {
        size_t level, content;
        QwMarkdownFence fence;
        QwMarkdownListMarker marker;
        int rc;
        if (markdown_line_blank(&line)) continue;
        if (markdown_fence(&line, &fence)) {
            rc = markdown_render_fence(data, size, &position, &fence, html);
            if (rc != QW_STATUS_OK) return rc;
            continue;
        }
        if (markdown_heading(&line, &level, &content)) {
            rc = markdown_render_heading(&line, level, content, html,
                                         removed_images);
            if (rc != QW_STATUS_OK) return rc;
            continue;
        }
        if (markdown_horizontal_rule(&line)) {
            rc = markdown_append_literal(html, "<hr />\n");
            if (rc != QW_STATUS_OK) return rc;
            continue;
        }
        if (markdown_blockquote(&line, &content)) {
            rc = markdown_render_blockquote(data, size, &position, &line,
                                            html, removed_images, depth);
            if (rc != QW_STATUS_OK) return rc;
            continue;
        }
        if (markdown_list_marker(&line, &marker)) {
            rc = markdown_render_list(data, size, &position, &line,
                                      html, removed_images);
            if (rc != QW_STATUS_OK) return rc;
            continue;
        }
        {
            size_t indent = markdown_leading_spaces(&line);
            if (indent == 4u) {
                rc = markdown_append_literal(html, "<pre><code>");
                if (rc != QW_STATUS_OK) return rc;
                for (;;) {
                    rc = markdown_escape_html(html, line.data + 4u,
                                              line.size - 4u, 0);
                    if (rc == QW_STATUS_OK)
                        rc = markdown_append(html, "\n", 1u);
                    if (rc != QW_STATUS_OK) return rc;
                    if (position >= size) break;
                    {
                        size_t before = position;
                        size_t next_position = before;
                        if (markdown_read_line(data, size, &next_position,
                                               &line) <= 0) break;
                        if (markdown_line_blank(&line)) {
                            position = next_position;
                            rc = markdown_append(html, "\n", 1u);
                            if (rc != QW_STATUS_OK) return rc;
                            continue;
                        }
                        if (markdown_leading_spaces(&line) < 4u) {
                            position = before;
                            break;
                        }
                        position = next_position;
                    }
                }
                rc = markdown_append_literal(html, "</code></pre>\n");
                if (rc != QW_STATUS_OK) return rc;
                continue;
            }
        }
        {
            QwTextBuffer paragraph = {0};
            paragraph.limit = QW_MARKDOWN_PARSE_MAX_INPUT + 1u;
            rc = markdown_append(&paragraph, (const char *)line.data,
                                 line.size);
            if (rc != QW_STATUS_OK) {
                free(paragraph.data);
                return rc;
            }
            while (position < size) {
                size_t before = position;
                size_t next_position = before;
                if (markdown_read_line(data, size, &next_position, &line) <= 0)
                    break;
                if (markdown_line_blank(&line) ||
                    markdown_line_starts_block(&line)) {
                    position = before;
                    break;
                }
                rc = markdown_append(&paragraph, "\n", 1u);
                if (rc == QW_STATUS_OK)
                    rc = markdown_append(&paragraph, (const char *)line.data,
                                         line.size);
                if (rc != QW_STATUS_OK) {
                    free(paragraph.data);
                    return rc;
                }
                position = next_position;
            }
            rc = markdown_append_literal(html, "<p>");
            if (rc == QW_STATUS_OK)
                rc = markdown_inline((const uint8_t *)paragraph.data,
                                     paragraph.size, html,
                                     removed_images, 0u);
            if (rc == QW_STATUS_OK) rc = markdown_append_literal(html, "</p>\n");
            free(paragraph.data);
            if (rc != QW_STATUS_OK) return rc;
        }
    }
    return QW_STATUS_OK;
}

int qw_markdown_probe(const void *source, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)source;
    size_t scan_size;
    size_t position = 0u;
    QwMarkdownLine line;
    if (!bytes || !size) return 0;
    scan_size = size < QW_MARKDOWN_PROBE_LIMIT
        ? size : QW_MARKDOWN_PROBE_LIMIT;
    if (scan_size >= 3u && bytes[0] == 0xefu && bytes[1] == 0xbbu &&
        bytes[2] == 0xbfu) position = 3u;
    for (size_t i = position; i < scan_size; ++i) {
        if (bytes[i] == 0u || bytes[i] < 0x09u ||
            (bytes[i] > 0x0du && bytes[i] < 0x20u)) return 0;
    }
    while (position < scan_size &&
           markdown_read_line(bytes, scan_size, &position, &line) > 0) {
        if (markdown_line_starts_block(&line)) return 1;
        for (size_t i = 0u; i + 1u < line.size; ++i) {
            if ((line.data[i] == '[' &&
                 memchr(line.data + i + 1u, ']',
                        line.size - i - 1u) != NULL) ||
                (line.data[i] == '*' && line.data[i + 1u] == '*') ||
                (line.data[i] == '_' && line.data[i + 1u] == '_') ||
                (line.data[i] == (uint8_t)0x60u &&
                 memchr(line.data + i + 1u, (uint8_t)0x60u,
                        line.size - i - 1u) != NULL) ||
                ((line.data[i] == '*' || line.data[i] == '_') &&
                 memchr(line.data + i + 1u, line.data[i],
                        line.size - i - 1u) != NULL)) return 1;
        }
    }
    return 0;
}

int qw_markdown_parse_html(const uint8_t *source, size_t size,
                           QwTextBuffer *html_body,
                           size_t *out_removed_images)
{
    if (!source || !size || !html_body || !out_removed_images)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (size > QW_MARKDOWN_PARSE_MAX_INPUT) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (size >= 3u && source[0] == 0xefu &&
        source[1] == 0xbbu && source[2] == 0xbfu) {
        source += 3u;
        size -= 3u;
    }
    if (!markdown_utf8_valid(source, size)) return QW_STATUS_ERR_FORMAT;
    *out_removed_images = 0u;
    return markdown_render_blocks(source, size, html_body,
                                   out_removed_images, 0u);
}

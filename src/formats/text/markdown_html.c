/* HTML fragment -> Markdown writer.
 *
 * The readers in the document, eBook and text packages all normalize their
 * content to an HTML fragment. This module turns that de facto layout
 * intermediate into Markdown: headings, paragraphs, emphasis, links, lists,
 * blockquotes, inline and fenced code, simple tables and images. Script, style
 * and head content is dropped, images become their alt text, and unknown tags
 * are ignored (their text survives).
 *
 * This is deliberately a line-oriented approximation, not a full HTML parser:
 * the supported subset is the markup the Quirewise readers emit.
 */

#include "quirewise/markdown.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QW_HTML_TO_MD_INPUT_MAX ((size_t)256u * 1024u * 1024u)
#define QW_HTML_TO_MD_OUTPUT_MAX ((size_t)64u * 1024u * 1024u)

typedef struct MdOut {
    char *data;
    size_t size;
    size_t capacity;
    int failed;
} MdOut;

static void md_out_reset(MdOut *out)
{
    free(out->data);
    out->data = NULL;
    out->size = 0u;
    out->capacity = 0u;
    out->failed = 0;
}

static int md_out_reserve(MdOut *out, size_t additional)
{
    size_t needed;
    size_t capacity;
    char *grown;

    if (out->failed) return 0;
    if (out->size > QW_HTML_TO_MD_OUTPUT_MAX ||
        additional > QW_HTML_TO_MD_OUTPUT_MAX - out->size) {
        out->failed = 1;
        return 0;
    }
    needed = out->size + additional + 1u;
    if (needed <= out->capacity) return 1;
    capacity = out->capacity ? out->capacity : 4096u;
    while (capacity < needed) {
        if (capacity > QW_HTML_TO_MD_OUTPUT_MAX / 2u) {
            capacity = QW_HTML_TO_MD_OUTPUT_MAX;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed) {
        out->failed = 1;
        return 0;
    }
    grown = (char *)realloc(out->data, capacity);
    if (!grown) {
        out->failed = 1;
        return 0;
    }
    out->data = grown;
    out->capacity = capacity;
    return 1;
}

static int md_append(MdOut *out, const char *text, size_t size)
{
    if (!size) return 1;
    if (!md_out_reserve(out, size)) return 0;
    memcpy(out->data + out->size, text, size);
    out->size += size;
    out->data[out->size] = '\0';
    return 1;
}

static int md_byte(MdOut *out, char value)
{
    return md_append(out, &value, 1u);
}

static int md_at_line_start(const MdOut *out)
{
    return out->size == 0u || out->data[out->size - 1u] == '\n';
}

static int md_newline(MdOut *out)
{
    if (md_at_line_start(out)) return 1;
    return md_byte(out, '\n');
}

/* At most one blank line, unless the buffer is empty. */
static int md_blank(MdOut *out)
{
    while (out->size > 0u && out->data[out->size - 1u] == (char)' ')
        --out->size;
    if (out->size == 0u) return 1;
    if (out->data[out->size - 1u] != '\n') {
        if (!md_byte(out, '\n')) return 0;
    }
    if (out->size >= 2u && out->data[out->size - 2u] == '\n') return 1;
    return md_byte(out, '\n');
}

static int md_space(MdOut *out)
{
    size_t size = out->size;

    if (size == 0u) return 1;
    if (out->data[size - 1u] == ' ' || out->data[size - 1u] == '\n')
        return 1;
    return md_byte(out, ' ');
}

static int md_hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t utf8_encode(uint32_t codepoint, char out[4])
{
    if (codepoint <= 0x7fu) {
        out[0] = (char)codepoint;
        return 1u;
    }
    if (codepoint <= 0x7ffu) {
        out[0] = (char)(0xc0u | (codepoint >> 6));
        out[1] = (char)(0x80u | (codepoint & 0x3fu));
        return 2u;
    }
    if (codepoint <= 0xffffu) {
        out[0] = (char)(0xe0u | (codepoint >> 12));
        out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        out[2] = (char)(0x80u | (codepoint & 0x3fu));
        return 3u;
    }
    if (codepoint <= 0x10ffffu) {
        out[0] = (char)(0xf0u | (codepoint >> 18));
        out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
        out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        out[3] = (char)(0x80u | (codepoint & 0x3fu));
        return 4u;
    }
    return 0u;
}

/* Decode one entity starting at '&'. Returns the bytes consumed, or 0 when the
 * run is not a recognized entity. */
static size_t md_decode_entity(const uint8_t *data, size_t size, char out[8],
                               size_t *out_size)
{
    size_t index = 1u;
    uint32_t codepoint = 0u;
    int has_digits = 0;

    if (index >= size) return 0u;
    if (data[index] == '#') {
        index += 1u;
        if (index < size && (data[index] == 'x' || data[index] == 'X')) {
            index += 1u;
            while (index < size && md_hex_value(data[index]) >= 0) {
                codepoint = codepoint * 16u +
                            (uint32_t)md_hex_value(data[index]);
                index += 1u;
                has_digits = 1;
                if (codepoint > 0x10ffffu) break;
            }
        } else {
            while (index < size && data[index] >= '0' && data[index] <= '9') {
                codepoint = codepoint * 10u + (uint32_t)(data[index] - '0');
                index += 1u;
                has_digits = 1;
                if (codepoint > 0x10ffffu) break;
            }
        }
        if (!has_digits || index >= size || data[index] != ';') return 0u;
        index += 1u;
        *out_size = utf8_encode(codepoint, out);
        return *out_size ? index : 0u;
    }
    {
        static const struct {
            const char *name;
            uint32_t codepoint;
        } named[] = {
            { "amp;", 0x26u }, { "lt;", 0x3cu }, { "gt;", 0x3eu },
            { "quot;", 0x22u }, { "apos;", 0x27u }, { "nbsp;", 0x20u },
            { "mdash;", 0x2014u }, { "ndash;", 0x2013u },
            { "hellip;", 0x2026u }, { "copy;", 0xa9u }
        };
        size_t entry;

        for (entry = 0u; entry < sizeof(named) / sizeof(named[0]); ++entry) {
            size_t name_size = strlen(named[entry].name);
            if (index + name_size <= size &&
                memcmp(data + index, named[entry].name, name_size) == 0) {
                *out_size = utf8_encode(named[entry].codepoint, out);
                return index + name_size;
            }
        }
    }
    return 0u;
}

typedef struct MdTag {
    char name[16];
    int closing;
    int self_closing;
    char href[1024];
    char alt[512];
    char src[4096];
    char title[512];
    char align[16];
    char style[256];
    size_t href_size;
    size_t alt_size;
    size_t src_size;
} MdTag;

static int md_name_equal(const uint8_t *name, size_t size, const char *expected)
{
    size_t index;

    if (strlen(expected) != size) return 0;
    for (index = 0u; index < size; ++index) {
        char value = (char)((name[index] >= 'A' && name[index] <= 'Z')
                                ? name[index] - 'A' + 'a' : name[index]);
        if (value != expected[index]) return 0;
    }
    return 1;
}

static size_t md_find_tag_end(const uint8_t *data, size_t size, size_t start)
{
    size_t index = start;

    for (; index < size; ++index) {
        if (data[index] == '>') return index;
    }
    return size;
}

static size_t md_skip_quoted(const uint8_t *data, size_t size, size_t start,
                             char quote, char *out, size_t capacity)
{
    size_t index = start + 1u;
    size_t written = 0u;

    for (; index < size && data[index] != quote; ++index) {
        if (written + 1u < capacity) out[written++] = (char)data[index];
    }
    if (index < size) ++index;
    out[written < capacity ? written : capacity - 1u] = '\0';
    return index;
}

/* Parse one tag at data[start] == '<'. Returns bytes consumed, 0 when the input
 * should be treated as text. */
static size_t md_parse_tag(const uint8_t *data, size_t size, size_t start,
                           MdTag *tag)
{
    size_t index = start + 1u;
    size_t name_start;
    size_t end;

    memset(tag, 0, sizeof(*tag));
    if (index < size && data[index] == '!') {
        /* Comment, doctype or declaration: consume through the next '>'. */
        if (index + 1u < size && data[index + 1u] == '-' &&
            index + 2u < size && data[index + 2u] == '-') {
            index += 3u;
            while (index + 2u < size && !(data[index] == '-' &&
                                          data[index + 1u] == '-' &&
                                          data[index + 2u] == '>'))
                ++index;
            if (index + 2u >= size) return size - start;
            return index + 3u - start;
        }
        return md_find_tag_end(data, size, index) + 1u - start;
    }
    if (index < size && data[index] == '?') {
        return md_find_tag_end(data, size, index) + 1u - start;
    }
    if (index < size && data[index] == '/') {
        tag->closing = 1;
        ++index;
    }
    name_start = index;
    while (index < size &&
           ((data[index] >= 'a' && data[index] <= 'z') ||
            (data[index] >= 'A' && data[index] <= 'Z') ||
            (data[index] >= '0' && data[index] <= '9'))) {
        if (index - name_start < sizeof(tag->name) - 1u)
            tag->name[index - name_start] = (char)data[index];
        ++index;
    }
    tag->name[index - name_start < sizeof(tag->name) - 1u
                    ? index - name_start : sizeof(tag->name) - 1u] = '\0';
    if (!tag->name[0]) return 0u;
    end = md_find_tag_end(data, size, index);
    if (end >= size) return 0u;
    /* Attributes we understand: href and alt. */
    while (index < end) {
        while (index < end && (data[index] == ' ' || data[index] == '\t' ||
                               data[index] == '\r' || data[index] == '\n'))
            ++index;
        if (index >= end) break;
        if (data[index] == '/') {
            tag->self_closing = 1;
            ++index;
            continue;
        }
        if ((data[index] >= 'a' && data[index] <= 'z') ||
            (data[index] >= 'A' && data[index] <= 'Z')) {
            size_t attr_start = index;
            size_t attr_size;

            while (index < end &&
                   ((data[index] >= 'a' && data[index] <= 'z') ||
                    (data[index] >= 'A' && data[index] <= 'Z') ||
                    (data[index] >= '0' && data[index] <= '9') ||
                    data[index] == '-' || data[index] == '_'))
                ++index;
            attr_size = index - attr_start;
            while (index < end && (data[index] == ' ' || data[index] == '\t'))
                ++index;
            if (index < end && data[index] == '=') {
                ++index;
                while (index < end && (data[index] == ' ' ||
                                       data[index] == '\t'))
                    ++index;
                if (index < end &&
                    (data[index] == '"' || data[index] == '\'')) {
                    char quote = (char)data[index];
                    if (attr_size == 4u &&
                        md_name_equal(data + attr_start, 4u, "href")) {
                        index = md_skip_quoted(data, end, index, quote,
                                               tag->href,
                                               sizeof(tag->href));
                    } else if (attr_size == 3u &&
                               md_name_equal(data + attr_start, 3u, "alt")) {
                        index = md_skip_quoted(data, end, index, quote,
                                               tag->alt,
                                               sizeof(tag->alt));
                    } else if (attr_size == 3u &&
                               md_name_equal(data + attr_start, 3u, "src")) {
                        index = md_skip_quoted(data, end, index, quote,
                                               tag->src,
                                               sizeof(tag->src));
                    } else if (attr_size == 5u &&
                               md_name_equal(data + attr_start, 5u, "title")) {
                        index = md_skip_quoted(data, end, index, quote,
                                               tag->title,
                                               sizeof(tag->title));
                    } else if (attr_size == 5u &&
                               md_name_equal(data + attr_start, 5u,
                                             "align")) {
                        index = md_skip_quoted(data, end, index, quote,
                                               tag->align,
                                               sizeof(tag->align));
                    } else if (attr_size == 5u &&
                               md_name_equal(data + attr_start, 5u,
                                             "style")) {
                        index = md_skip_quoted(data, end, index, quote,
                                               tag->style,
                                               sizeof(tag->style));
                    } else {
                        char discard[2];
                        index = md_skip_quoted(data, end, index, quote,
                                               discard, sizeof(discard));
                    }
                } else {
                    while (index < end && data[index] != ' ' &&
                           data[index] != '>')
                        ++index;
                }
            }
        } else {
            ++index;
        }
    }
    return end + 1u - start;
}

static size_t md_find_close_tag(const uint8_t *data, size_t size, size_t start,
                                const char *name)
{
    size_t index = start;
    size_t name_size = strlen(name);

    for (; index + name_size + 3u <= size; ++index) {
        if (data[index] == '<' && data[index + 1u] == '/') {
            size_t candidate = index + 2u;
            if (md_name_equal(data + candidate, name_size, name)) {
                while (candidate < size && data[candidate] != '>')
                    ++candidate;
                return candidate < size ? candidate + 1u : size;
            }
        }
    }
    return size;
}

static int md_escape_text(MdOut *out, const uint8_t *data, size_t size,
                          int in_pre, int quote_depth)
{
    size_t index = 0u;
    int saw_space = 0;

    while (index < size) {
        uint8_t value = data[index];
        char decoded[8];
        size_t decoded_size = 0u;

        if (in_pre) {
            if (!md_append(out, (const char *)&value, 1u)) return 0;
            ++index;
            continue;
        }
        if (value == '\r' || value == '\n' || value == '\t' || value == ' ') {
            saw_space = 1;
            ++index;
            continue;
        }
        if (saw_space) {
            if (!md_at_line_start(out) && !md_space(out)) return 0;
            saw_space = 0;
        }
        if (md_at_line_start(out)) {
            int depth;
            for (depth = 0; depth < quote_depth; ++depth) {
                if (!md_append(out, "> ", 2u)) return 0;
            }
        }
        if (value == '&') {
            size_t consumed = md_decode_entity(data + index, size - index,
                                               decoded, &decoded_size);
            if (consumed) {
                if (decoded_size == 1u &&
                    (decoded[0] == '\\' || decoded[0] == '`')) {
                    if (!md_byte(out, '\\')) return 0;
                }
                if (!md_append(out, decoded, decoded_size)) return 0;
                index += consumed;
                continue;
            }
        }
        if (value == '\\' || value == '`') {
            if (!md_byte(out, '\\')) return 0;
        }
        if (!md_byte(out, (char)value)) return 0;
        ++index;
    }
    /* Keep a single trailing space so an inline tag that follows a word break
     * does not glue itself to the previous word. Block breaks trim it later. */
    if (saw_space && !md_at_line_start(out) && !md_space(out)) return 0;
    return 1;
}

static int md_push_list(int *stack, int *depth, int ordered)
{
    if (*depth >= 8) return 1;
    stack[*depth] = ordered;
    ++*depth;
    return 1;
}

static int md_pop_list(int *depth)
{
    if (*depth > 0) --*depth;
    return 1;
}

static int md_open_heading(MdOut *out, int level)
{
    int index;

    if (!md_blank(out)) return 0;
    for (index = 0; index < level; ++index) {
        if (!md_byte(out, '#')) return 0;
    }
    return md_byte(out, ' ');
}

QwMarkdownStatus qw_markdown_from_html(const void *html, size_t size,
                                      QwMarkdownText *out_document)
{
    const uint8_t *data = (const uint8_t *)html;
    MdOut out;
    size_t index = 0u;
    int in_pre = 0;
    int quote_depth = 0;
    int list_depth = 0;
    int list_stack[8];
    int link_open = 0;
    char link_href[1024];
    size_t link_href_size = 0u;
    int table_open = 0;
    int table_row_open = 0;
    int table_header_done = 0;
    int table_cell_open = 0;
    int table_cells = 0;
    int table_align[64];
    int table_align_columns = 0;
    size_t removed_images = 0u;

    if (!out_document) return QW_MARKDOWN_ERR_INVALID_ARGUMENT;
    out_document->markdown = NULL;
    out_document->markdown_size = 0u;
    out_document->removed_images = 0u;
    if (!data || !size) return QW_MARKDOWN_ERR_INVALID_ARGUMENT;
    if (size > QW_HTML_TO_MD_INPUT_MAX) return QW_MARKDOWN_ERR_RESOURCE_LIMIT;

    memset(&out, 0, sizeof(out));
    while (index < size) {
        if (data[index] != '<') {
            size_t start = index;
            while (index < size && data[index] != '<') ++index;
            if (!md_escape_text(&out, data + start, index - start, in_pre,
                                quote_depth))
                goto out_of_memory;
            continue;
        }
        {
            MdTag tag;
            size_t consumed = md_parse_tag(data, size, index, &tag);
            if (!consumed) {
                if (!md_byte(&out, '<')) goto out_of_memory;
                ++index;
                continue;
            }
            index += consumed;
            if (!tag.name[0]) continue;
            if (md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "script") ||
                md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "style") ||
                md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "title")) {
                if (!tag.closing)
                    index = md_find_close_tag(data, size, index, tag.name);
                continue;
            }
            if (md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "head")) {
                if (!tag.closing)
                    index = md_find_close_tag(data, size, index, "head");
                continue;
            }
            if (tag.closing) {
                if (md_name_equal((const uint8_t *)tag.name,
                                  strlen(tag.name), "h1") ||
                    md_name_equal((const uint8_t *)tag.name,
                                  strlen(tag.name), "h2") ||
                    md_name_equal((const uint8_t *)tag.name,
                                  strlen(tag.name), "h3") ||
                    md_name_equal((const uint8_t *)tag.name,
                                  strlen(tag.name), "h4") ||
                    md_name_equal((const uint8_t *)tag.name,
                                  strlen(tag.name), "h5") ||
                    md_name_equal((const uint8_t *)tag.name,
                                  strlen(tag.name), "h6")) {
                    if (!md_blank(&out)) goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "p") ||
                           md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "div")) {
                    if (!md_blank(&out)) goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "strong") ||
                           md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "b")) {
                    if (!md_append(&out, "**", 2u)) goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "em") ||
                           md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "i")) {
                    if (!md_byte(&out, '*')) goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "code")) {
                    if (!in_pre && !md_byte(&out, '`')) goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "pre")) {
                    if (in_pre) {
                        if (!md_append(&out, "\n```\n\n", 6u))
                            goto out_of_memory;
                        in_pre = 0;
                    }
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "a")) {
                    if (link_open) {
                        if (!md_append(&out, "](", 2u) ||
                            !md_append(&out, link_href, link_href_size) ||
                            !md_byte(&out, ')'))
                            goto out_of_memory;
                        link_open = 0;
                        link_href_size = 0u;
                    }
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "del") ||
                           md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "s") ||
                           md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "strike")) {
                    if (!md_append(&out, "~~", 2u)) goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "sup") ||
                           md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "sub")) {
                    const char *close = md_name_equal((const uint8_t *)tag.name,
                                                      strlen(tag.name), "sup")
                                            ? "</sup>" : "</sub>";
                    if (!md_append(&out, close, strlen(close)))
                        goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "ul") ||
                           md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "ol")) {
                    md_pop_list(&list_depth);
                    if (!md_blank(&out)) goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "li")) {
                    if (!md_newline(&out)) goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "blockquote")) {
                    if (quote_depth > 0) --quote_depth;
                    if (!md_blank(&out)) goto out_of_memory;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "tr")) {
                    if (table_row_open) {
                        if (!md_append(&out, " |\n", 3u))
                            goto out_of_memory;
                        table_row_open = 0;
                        table_cell_open = 0;
                    }
                    if (!table_header_done && table_cells > 0) {
                        int cell;
                        if (!md_byte(&out, '|')) goto out_of_memory;
                        for (cell = 0; cell < table_cells; ++cell) {
                            int alignment = cell < table_align_columns
                                                ? table_align[cell] : 0;
                            const char *separator =
                                alignment == 3 ? " :---: |"
                                : alignment == 2 ? " ---: |"
                                : alignment == 1 ? " :--- |"
                                                 : " --- |";
                            if (!md_append(&out, separator,
                                           strlen(separator)))
                                goto out_of_memory;
                        }
                        if (!md_byte(&out, '\n')) goto out_of_memory;
                        table_header_done = 1;
                    }
                    table_cells = 0;
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "td") ||
                           md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "th")) {
                    /* The next cell opening adds the separator. */
                } else if (md_name_equal((const uint8_t *)tag.name,
                                         strlen(tag.name), "table")) {
                    if (!md_blank(&out)) goto out_of_memory;
                    table_open = 0;
                    table_header_done = 0;
                }
                continue;
            }
            if (md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "h1") ||
                md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "h2") ||
                md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "h3") ||
                md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "h4") ||
                md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "h5") ||
                md_name_equal((const uint8_t *)tag.name, strlen(tag.name),
                              "h6")) {
                int level = tag.name[1] - '0';
                if (!md_open_heading(&out, level)) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "p") ||
                       md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "div")) {
                if (!md_blank(&out)) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "hr")) {
                if (!md_blank(&out) || !md_append(&out, "---\n", 4u))
                    goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "dl") ||
                       md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "dt")) {
                if (!md_blank(&out)) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "dd")) {
                if (!md_newline(&out) || !md_append(&out, ": ", 2u))
                    goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "br")) {
                if (!md_newline(&out)) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "strong") ||
                       md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "b")) {
                if (!md_append(&out, "**", 2u)) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "em") ||
                       md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "i")) {
                if (!md_byte(&out, '*')) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "code")) {
                if (!in_pre && !md_byte(&out, '`')) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "pre")) {
                if (!md_blank(&out) || !md_append(&out, "```\n", 4u))
                    goto out_of_memory;
                in_pre = 1;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "a")) {
                link_open = 1;
                link_href_size = strlen(tag.href);
                if (link_href_size >= sizeof(link_href))
                    link_href_size = sizeof(link_href) - 1u;
                memcpy(link_href, tag.href, link_href_size);
                link_href[link_href_size] = '\0';
                if (!md_byte(&out, '[')) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "del") ||
                       md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "s") ||
                       md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "strike")) {
                if (!md_append(&out, "~~", 2u)) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "sup") ||
                       md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "sub")) {
                const char *open = md_name_equal((const uint8_t *)tag.name,
                                                 strlen(tag.name), "sup")
                                           ? "<sup>" : "<sub>";
                if (!md_append(&out, open, strlen(open)))
                    goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "ul") ||
                       md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "ol")) {
                int ordered =
                    md_name_equal((const uint8_t *)tag.name,
                                  strlen(tag.name), "ol");
                md_push_list(list_stack, &list_depth, ordered);
                if (!md_blank(&out)) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "li")) {
                if (!md_newline(&out)) goto out_of_memory;
                if (!md_append(&out, list_depth > 0 && list_stack[list_depth - 1]
                                          ? "1. " : "- ",
                              list_depth > 0 && list_stack[list_depth - 1]
                                  ? 3u : 2u))
                    goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "blockquote")) {
                ++quote_depth;
                if (!md_blank(&out)) goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "table")) {
                if (!md_blank(&out)) goto out_of_memory;
                table_open = 1;
                table_header_done = 0;
                table_align_columns = 0;
                memset(table_align, 0, sizeof(table_align));
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "tr")) {
                if (!md_at_line_start(&out) && !md_newline(&out))
                    goto out_of_memory;
                if (!md_append(&out, "| ", 2u)) goto out_of_memory;
                table_row_open = 1;
                table_cells = 0;
                table_cell_open = 0;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "td") ||
                       md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "th")) {
                if (table_row_open && table_cells > 0) {
                    if (!md_append(&out, " | ", 3u))
                        goto out_of_memory;
                }
                if (!table_header_done && table_cells < 64) {
                    int alignment = 0;
                    const char *align_text = tag.align;
                    char style_align[16];
                    style_align[0] = '\0';
                    if (!align_text[0] && tag.style[0]) {
                        const char *style_attr = strstr(tag.style,
                                                        "text-align");
                        if (style_attr) {
                            const char *colon = strchr(style_attr, ':');
                            if (colon) {
                                size_t copy = 0u;
                                ++colon;
                                while (*colon == ' ' || *colon == '\t')
                                    ++colon;
                                while (colon[copy] && colon[copy] != ';' &&
                                       colon[copy] != ' ' &&
                                       colon[copy] != '\t' &&
                                       copy + 1u < sizeof(style_align)) {
                                    style_align[copy] = colon[copy];
                                    ++copy;
                                }
                                style_align[copy] = '\0';
                                align_text = style_align;
                            }
                        }
                    }
                    if (align_text[0]) {
                        if (md_name_equal((const uint8_t *)align_text,
                                          strlen(align_text), "center"))
                            alignment = 3;
                        else if (md_name_equal((const uint8_t *)align_text,
                                               strlen(align_text), "right"))
                            alignment = 2;
                        else if (md_name_equal((const uint8_t *)align_text,
                                               strlen(align_text), "left"))
                            alignment = 1;
                    }
                    table_align[table_cells] = alignment;
                    if (table_cells + 1 > table_align_columns)
                        table_align_columns = table_cells + 1;
                }
                ++table_cells;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "hr")) {
                if (!md_blank(&out) || !md_append(&out, "---\n", 4u))
                    goto out_of_memory;
            } else if (md_name_equal((const uint8_t *)tag.name,
                                     strlen(tag.name), "img")) {
                if (tag.src[0]) {
                    if (!md_append(&out, "![", 2u) ||
                        (tag.alt[0] &&
                         !md_escape_text(&out, (const uint8_t *)tag.alt,
                                         strlen(tag.alt), 0, quote_depth)) ||
                        !md_append(&out, "](", 2u) ||
                        !md_append(&out, tag.src, strlen(tag.src)) ||
                        (tag.title[0] &&
                         (!md_append(&out, " \"", 2u) ||
                          !md_escape_text(&out,
                                          (const uint8_t *)tag.title,
                                          strlen(tag.title), 0,
                                          quote_depth) ||
                          !md_append(&out, "\"", 1u))) ||
                        !md_append(&out, ")", 1u))
                        goto out_of_memory;
                } else {
                    ++removed_images;
                    if (tag.alt[0]) {
                        if (!md_escape_text(&out, (const uint8_t *)tag.alt,
                                            strlen(tag.alt), 0, quote_depth))
                            goto out_of_memory;
                    }
                }
            }
        }
    }
    (void)table_open;
    (void)table_header_done;
    /* Trim trailing whitespace, keep exactly one final newline. */
    while (out.size > 0u &&
           (out.data[out.size - 1u] == ' ' || out.data[out.size - 1u] == '\n'))
        --out.size;
    if (out.size > 0u && !md_byte(&out, '\n')) goto out_of_memory;
    if (!out.data) {
        out.data = (char *)malloc(1u);
        if (!out.data) goto out_of_memory;
        out.data[0] = '\0';
    }
    out_document->markdown = out.data;
    out_document->markdown_size = out.size;
    out_document->removed_images = removed_images;
    return QW_MARKDOWN_OK;

out_of_memory:
    md_out_reset(&out);
    return QW_MARKDOWN_ERR_OUT_OF_MEMORY;
}

void qw_markdown_text_free(QwMarkdownText *document)
{
    if (!document) return;
    free(document->markdown);
    document->markdown = NULL;
    document->markdown_size = 0u;
    document->removed_images = 0u;
}

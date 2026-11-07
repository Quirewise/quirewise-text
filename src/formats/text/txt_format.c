#include "format_registry.h"
#include "../../core/byte_utils.h"
#include "txt_format.h"
#include <quirewise/charset.h>

#ifndef QUIREWISE_STANDALONE_PLAIN_TEXT_PARSER
#include "../../pdf/pdf_internal.h"
#include "quirewise/pml.h"
#include "quirewise/text_archive.h"
#endif

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct QwHtmlBuffer {
    char *data;
    size_t size;
    size_t capacity;
} QwHtmlBuffer;

static int html_append(QwHtmlBuffer *buffer, const char *data, size_t size)
{
    size_t capacity;
    char *grown;
    if (size > SIZE_MAX - buffer->size - 1u) return 0;
    if (buffer->size + size + 1u > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 1024u;
        while (capacity < buffer->size + size + 1u) {
            if (capacity > SIZE_MAX / 2u) return 0;
            capacity *= 2u;
        }
        grown = (char *)realloc(buffer->data, capacity);
        if (!grown) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int html_append_literal(QwHtmlBuffer *buffer, const char *text)
{
    return html_append(buffer, text, strlen(text));
}

static int html_append_codepoint(QwHtmlBuffer *buffer, unsigned long cp)
{
    char encoded[4];
    size_t count;
    if (cp <= 0x7fu) {
        encoded[0] = (char)cp;
        count = 1;
    } else if (cp <= 0x7ffu) {
        encoded[0] = (char)(0xc0u | (cp >> 6));
        encoded[1] = (char)(0x80u | (cp & 0x3fu));
        count = 2;
    } else if (cp <= 0xffffu && !(cp >= 0xd800u && cp <= 0xdfffu)) {
        encoded[0] = (char)(0xe0u | (cp >> 12));
        encoded[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        encoded[2] = (char)(0x80u | (cp & 0x3fu));
        count = 3;
    } else if (cp <= 0x10ffffu) {
        encoded[0] = (char)(0xf0u | (cp >> 18));
        encoded[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
        encoded[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        encoded[3] = (char)(0x80u | (cp & 0x3fu));
        count = 4;
    } else {
        return html_append_literal(buffer, "\xef\xbf\xbd");
    }
    return html_append(buffer, encoded, count);
}

static unsigned long read_u16(const uint8_t *data, int big_endian)
{
    return big_endian ? ((unsigned long)data[0] << 8) | data[1]
                      : ((unsigned long)data[1] << 8) | data[0];
}

static int txt_is_valid_utf8(const uint8_t *data, size_t size)
{
    size_t i = 0;
    while (i < size) {
        unsigned char first = data[i];
        size_t count;
        if (first < 0x80u) {
            count = 1u;
        } else if (first >= 0xc2u && first <= 0xdfu) {
            count = 2u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            count = 3u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            count = 4u;
        } else {
            return 0;
        }
        if (count > size - i) return 0;
        for (size_t j = 1u; j < count; ++j) {
            if ((data[i + j] & 0xc0u) != 0x80u) return 0;
        }
        if ((first == 0xe0u && data[i + 1u] < 0xa0u) ||
            (first == 0xedu && data[i + 1u] >= 0xa0u) ||
            (first == 0xf0u && data[i + 1u] < 0x90u) ||
            (first == 0xf4u && data[i + 1u] >= 0x90u)) return 0;
        i += count;
    }
    return 1;
}

static int txt_decode_gbk(const uint8_t *data, size_t size,
                          uint8_t **out_data, size_t *out_size)
{
    QwCharsetStatus charset_status;
    size_t converted_size = 0u;
    uint8_t *utf8;
    *out_data = NULL;
    *out_size = 0;
    if (!size) return QW_STATUS_OK;
    charset_status = qw_charset_convert("GBK", "UTF-8", data, size,
                                        NULL, 0u, out_size);
    if (charset_status == QW_CHARSET_ERR_UNSUPPORTED_ENCODING ||
        charset_status == QW_CHARSET_ERR_RESOURCE_LIMIT ||
        *out_size == SIZE_MAX) return QW_STATUS_ERR_UNSUPPORTED;
    if (charset_status != QW_CHARSET_OK) return QW_STATUS_ERR_DECODE;
    utf8 = (uint8_t *)malloc(*out_size ? *out_size : 1u);
    if (!utf8) {
        *out_size = 0u;
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    charset_status = qw_charset_convert("GBK", "UTF-8", data, size,
                                        utf8, *out_size, &converted_size);
    if (charset_status != QW_CHARSET_OK || converted_size != *out_size) {
        free(utf8);
        *out_size = 0u;
        return charset_status == QW_CHARSET_ERR_RESOURCE_LIMIT
            ? QW_STATUS_ERR_UNSUPPORTED : QW_STATUS_ERR_DECODE;
    }
    *out_data = utf8;
    return QW_STATUS_OK;
}

static int text_to_html(QwHtmlBuffer *buffer, const uint8_t *data, size_t size)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        "@page{size:A4;margin:20mm}body{font-family:sans-serif;font-size:11pt}"
        "pre{white-space:pre-wrap;overflow-wrap:anywhere;font-family:sans-serif}"
        "</style></head><body><pre>";
    static const char suffix[] = "</pre></body></html>";
    size_t i = 0;
    int utf16 = 0;
    int big_endian = 0;

    if (!html_append(buffer, prefix, sizeof(prefix) - 1u)) return 0;
    if (size >= 2 && data[0] == 0xff && data[1] == 0xfe) {
        utf16 = 1;
        i = 2;
    } else if (size >= 2 && data[0] == 0xfe && data[1] == 0xff) {
        utf16 = 1;
        big_endian = 1;
        i = 2;
    } else if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
        i = 3;
    }
    if (utf16) {
        while (i + 1u < size) {
            unsigned long cp = read_u16(data + i, big_endian);
            i += 2u;
            if (cp >= 0xd800u && cp <= 0xdbffu) {
                if (i + 1u < size) {
                    unsigned long low = read_u16(data + i, big_endian);
                    if (low >= 0xdc00u && low <= 0xdfffu) {
                        cp = 0x10000u + ((cp - 0xd800u) << 10) + low - 0xdc00u;
                        i += 2u;
                    } else {
                        cp = 0xfffdu;
                    }
                } else {
                    cp = 0xfffdu;
                }
            } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
                cp = 0xfffdu;
            }
            if (cp == '&') {
                if (!html_append_literal(buffer, "&amp;")) return 0;
            } else if (cp == '<') {
                if (!html_append_literal(buffer, "&lt;")) return 0;
            } else if (cp == '>') {
                if (!html_append_literal(buffer, "&gt;")) return 0;
            } else if (!html_append_codepoint(buffer, cp)) {
                return 0;
            }
        }
        if (i < size && !html_append_literal(buffer, "\xef\xbf\xbd")) return 0;
    } else {
        while (i < size) {
            unsigned long cp;
            size_t count;
            unsigned char first = data[i];
            if (first < 0x80u) {
                cp = first;
                count = 1;
            } else if (first >= 0xc2u && first <= 0xdfu && i + 1u < size &&
                       (data[i + 1u] & 0xc0u) == 0x80u) {
                cp = ((unsigned long)(first & 0x1fu) << 6) | (data[i + 1u] & 0x3fu);
                count = 2;
            } else if (first >= 0xe0u && first <= 0xefu && i + 2u < size &&
                       (data[i + 1u] & 0xc0u) == 0x80u &&
                       (data[i + 2u] & 0xc0u) == 0x80u &&
                       !(first == 0xe0u && data[i + 1u] < 0xa0u) &&
                       !(first == 0xedu && data[i + 1u] >= 0xa0u)) {
                cp = ((unsigned long)(first & 0x0fu) << 12) |
                     ((unsigned long)(data[i + 1u] & 0x3fu) << 6) |
                     (data[i + 2u] & 0x3fu);
                count = 3;
            } else if (first >= 0xf0u && first <= 0xf4u && i + 3u < size &&
                       (data[i + 1u] & 0xc0u) == 0x80u &&
                       (data[i + 2u] & 0xc0u) == 0x80u &&
                       (data[i + 3u] & 0xc0u) == 0x80u &&
                       !(first == 0xf0u && data[i + 1u] < 0x90u) &&
                       !(first == 0xf4u && data[i + 1u] >= 0x90u)) {
                cp = ((unsigned long)(first & 0x07u) << 18) |
                     ((unsigned long)(data[i + 1u] & 0x3fu) << 12) |
                     ((unsigned long)(data[i + 2u] & 0x3fu) << 6) |
                     (data[i + 3u] & 0x3fu);
                count = 4;
            } else {
                cp = 0xfffdu;
                count = 1;
            }
            i += count;
            if (cp == '&') {
                if (!html_append_literal(buffer, "&amp;")) return 0;
            } else if (cp == '<') {
                if (!html_append_literal(buffer, "&lt;")) return 0;
            } else if (cp == '>') {
                if (!html_append_literal(buffer, "&gt;")) return 0;
            } else if ((cp < 0x20u && cp != '\n' && cp != '\r' && cp != '\t') ||
                       cp == 0x7fu) {
                if (!html_append_literal(buffer, " ")) return 0;
            } else if (!html_append_codepoint(buffer, cp)) {
                return 0;
            }
        }
    }
    return html_append(buffer, suffix, sizeof(suffix) - 1u);
}

static int txt_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    int utf16;
    size_t i;
    if (!bytes || !size || (size >= 4 && memcmp(bytes, "PK\003\004", 4) == 0) ||
        (size >= 5 && memcmp(bytes, "%PDF-", 5) == 0)) return 0;
    utf16 = size >= 2 && ((bytes[0] == 0xff && bytes[1] == 0xfe) ||
                          (bytes[0] == 0xfe && bytes[1] == 0xff));
    if (utf16) {
        for (i = 2; i + 1u < size; i += 2u) {
            unsigned long cp = read_u16(bytes + i, bytes[0] == 0xfe);
            if (cp == 0) continue;
            if (cp < 0x20u && cp != '\n' && cp != '\r' && cp != '\t' && cp != '\f') return 0;
        }
        return 1;
    }
    for (i = 0; i < size; ++i) {
        unsigned char ch = bytes[i];
        if (ch == 0 || (ch < 0x20u && ch != '\n' && ch != '\r' &&
                        ch != '\t' && ch != '\f' && ch != '\b')) return 0;
    }
    return 1;
}

#ifdef QUIREWISE_STANDALONE_PLAIN_TEXT_PARSER
int qw_txt_native_probe(const void *data, size_t size)
{
    return txt_probe(data, size);
}
#endif

#ifndef QUIREWISE_STANDALONE_PLAIN_TEXT_PARSER
typedef struct QwNativeTextPdfPage {
    PdfBuf content;
    size_t line_count;
} QwNativeTextPdfPage;

enum {
    QW_NATIVE_TEXT_LINE_COLUMNS = 84,
    QW_NATIVE_TEXT_LINES_PER_PAGE = 48
};

static void native_text_pages_free(QwNativeTextPdfPage *pages, size_t count)
{
    if (pages) {
        for (size_t i = 0; i < count; ++i) pdf_buf_free(&pages[i].content);
        free(pages);
    }
}

static int native_text_ensure_page(QwNativeTextPdfPage **pages,
                                   size_t *count, size_t *capacity)
{
    QwNativeTextPdfPage *grown;
    size_t new_capacity;
    if (*count && (*pages)[*count - 1u].line_count <
                      QW_NATIVE_TEXT_LINES_PER_PAGE) return 1;
    if (*count == *capacity) {
        new_capacity = *capacity ? *capacity * 2u : 4u;
        if (new_capacity < *capacity ||
            new_capacity > SIZE_MAX / sizeof(**pages)) return 0;
        grown = (QwNativeTextPdfPage *)realloc(
            *pages, new_capacity * sizeof(**pages));
        if (!grown) return 0;
        *pages = grown;
        *capacity = new_capacity;
    }
    memset(&(*pages)[*count], 0, sizeof(**pages));
    pdf_buf_init(&(*pages)[*count].content);
    pdf_buf_puts(&(*pages)[*count].content,
                 "BT\n/F1 11 Tf\n54 744 Td\n");
    ++*count;
    return !(*pages)[*count - 1u].content.oom;
}

static int native_text_emit_line(QwNativeTextPdfPage **pages,
                                 size_t *page_count, size_t *page_capacity,
                                 const uint8_t *line, size_t line_size)
{
    QwNativeTextPdfPage *page;
    if (!native_text_ensure_page(pages, page_count, page_capacity)) return 0;
    page = &(*pages)[*page_count - 1u];
    if (page->line_count) pdf_buf_puts(&page->content, "0 -14 Td\n");
    pdf_buf_puts(&page->content, "(");
    for (size_t i = 0; i < line_size; ++i) {
        uint8_t ch = line[i];
        if (ch == '(' || ch == ')' || ch == '\\') {
            pdf_buf_put(&page->content, "\\", 1u);
        }
        pdf_buf_put(&page->content, &ch, 1u);
    }
    pdf_buf_puts(&page->content, ") Tj\n");
    ++page->line_count;
    return !page->content.oom;
}

static uint32_t native_text_read_codepoint(const uint8_t *text, size_t size,
                                           size_t *used)
{
    uint8_t a;
    if (!size) {
        *used = 0;
        return 0xfffdu;
    }
    a = text[0];
    if (a < 0x80u) {
        *used = 1u;
        return a;
    }
    if (a >= 0xc2u && a <= 0xdfu && size >= 2u &&
        (text[1] & 0xc0u) == 0x80u) {
        *used = 2u;
        return ((uint32_t)(a & 0x1fu) << 6) | (text[1] & 0x3fu);
    }
    if (a >= 0xe0u && a <= 0xefu && size >= 3u &&
        (text[1] & 0xc0u) == 0x80u && (text[2] & 0xc0u) == 0x80u &&
        !(a == 0xe0u && text[1] < 0xa0u) &&
        !(a == 0xedu && text[1] >= 0xa0u)) {
        *used = 3u;
        return ((uint32_t)(a & 0x0fu) << 12) |
               ((uint32_t)(text[1] & 0x3fu) << 6) | (text[2] & 0x3fu);
    }
    if (a >= 0xf0u && a <= 0xf4u && size >= 4u &&
        (text[1] & 0xc0u) == 0x80u && (text[2] & 0xc0u) == 0x80u &&
        (text[3] & 0xc0u) == 0x80u &&
        !(a == 0xf0u && text[1] < 0x90u) &&
        !(a == 0xf4u && text[1] >= 0x90u)) {
        *used = 4u;
        return ((uint32_t)(a & 0x07u) << 18) |
               ((uint32_t)(text[1] & 0x3fu) << 12) |
               ((uint32_t)(text[2] & 0x3fu) << 6) | (text[3] & 0x3fu);
    }
    *used = 1u;
    return 0xfffdu;
}

static uint8_t native_text_winansi(uint32_t codepoint)
{
    switch (codepoint) {
    case 0x20acu: return 0x80u;
    case 0x201au: return 0x82u;
    case 0x0192u: return 0x83u;
    case 0x201eu: return 0x84u;
    case 0x2026u: return 0x85u;
    case 0x2020u: return 0x86u;
    case 0x2021u: return 0x87u;
    case 0x02c6u: return 0x88u;
    case 0x2030u: return 0x89u;
    case 0x0160u: return 0x8au;
    case 0x2039u: return 0x8bu;
    case 0x0152u: return 0x8cu;
    case 0x017du: return 0x8eu;
    case 0x2018u: return 0x91u;
    case 0x2019u: return 0x92u;
    case 0x201cu: return 0x93u;
    case 0x201du: return 0x94u;
    case 0x2022u: return 0x95u;
    case 0x2013u: return 0x96u;
    case 0x2014u: return 0x97u;
    case 0x02dcu: return 0x98u;
    case 0x2122u: return 0x99u;
    case 0x0161u: return 0x9au;
    case 0x203au: return 0x9bu;
    case 0x0153u: return 0x9cu;
    case 0x017eu: return 0x9eu;
    case 0x0178u: return 0x9fu;
    default:
        return codepoint <= 0xffu ? (uint8_t)codepoint : (uint8_t)'?';
    }
}

int qw_native_text_pdf_from_utf8(const uint8_t *text, size_t text_size,
                                uint8_t **out_pdf, size_t *out_size)
{
    QwNativeTextPdfPage *pages = NULL;
    size_t page_count = 0;
    size_t page_capacity = 0;
    uint8_t line[QW_NATIVE_TEXT_LINE_COLUMNS];
    size_t line_size = 0;
    size_t offset = 0;
    PdfWriter writer;
    int writer_initialized = 0;
    int rc = QW_STATUS_ERR_OUT_OF_MEMORY;
    if (!text || !out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0;
    if (text_size > 64u * 1024u * 1024u) return QW_STATUS_ERR_FORMAT;

    while (offset < text_size) {
        size_t used = 0;
        uint32_t cp = native_text_read_codepoint(text + offset,
                                                text_size - offset, &used);
        uint8_t ch;
        if (!used) break;
        offset += used;
        if (cp == '\r') continue;
        if (cp == '\n') {
            if (!native_text_emit_line(&pages, &page_count, &page_capacity,
                                       line, line_size)) goto cleanup;
            line_size = 0;
            continue;
        }
        if (cp == '\t') cp = ' ';
        if (cp < 0x20u || cp == 0x7fu) cp = ' ';
        ch = native_text_winansi(cp);
        if (line_size == sizeof(line)) {
            if (!native_text_emit_line(&pages, &page_count, &page_capacity,
                                       line, line_size)) goto cleanup;
            line_size = 0;
        }
        line[line_size++] = ch;
    }
    if (line_size || !page_count) {
        if (!native_text_emit_line(&pages, &page_count, &page_capacity,
                                   line, line_size)) goto cleanup;
    }
    for (size_t i = 0; i < page_count; ++i) {
        pdf_buf_puts(&pages[i].content, "ET\n");
        if (pages[i].content.oom) goto cleanup;
    }
    if (page_count > ((size_t)UINT32_MAX - 4u) / 2u) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }

    pdfw_init(&writer);
    writer_initialized = 1;
    pdf_buf_puts(&writer.buf, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    pdfw_start_object(&writer, 1u);
    pdfw_puts(&writer, "<</Type/Catalog/Pages 2 0 R>>");
    pdfw_end_object(&writer);
    pdfw_start_object(&writer, 2u);
    pdfw_printf(&writer, "<</Type/Pages/Count %zu/Kids[", page_count);
    for (size_t i = 0; i < page_count; ++i) {
        pdfw_printf(&writer, "%u 0 R ", (uint32_t)(4u + i * 2u));
    }
    pdfw_puts(&writer, "]>>");
    pdfw_end_object(&writer);
    pdfw_start_object(&writer, 3u);
    pdfw_puts(&writer,
              "<</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>>");
    pdfw_end_object(&writer);
    for (size_t i = 0; i < page_count; ++i) {
        uint32_t page_obj = (uint32_t)(4u + i * 2u);
        uint32_t content_obj = page_obj + 1u;
        pdfw_start_object(&writer, page_obj);
        pdfw_printf(&writer,
                    "<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]"
                    "/Resources<</Font<</F1 3 0 R>>>>/Contents %u 0 R>>",
                    content_obj);
        pdfw_end_object(&writer);
        pdfw_start_object(&writer, content_obj);
        pdfw_printf(&writer, "<</Length %zu>>\nstream\n",
                    pages[i].content.size);
        pdfw_put(&writer, pages[i].content.data, pages[i].content.size);
        pdfw_puts(&writer, "endstream");
        pdfw_end_object(&writer);
    }
    if (writer.buf.oom || !pdfw_finish_simple(&writer, 1u) || writer.buf.oom) {
        goto cleanup;
    }
    *out_pdf = writer.buf.data;
    *out_size = writer.buf.size;
    writer.buf.data = NULL;
    writer.buf.size = 0;
    writer.buf.cap = 0;
    rc = QW_STATUS_OK;

cleanup:
    if (writer_initialized) pdfw_free(&writer);
    native_text_pages_free(pages, page_count);
    return rc;
}

#endif

typedef struct QwTxtLayoutOwner {
    char *html;
} QwTxtLayoutOwner;

static void txt_layout_dispose(void *context)
{
    QwTxtLayoutOwner *owner = (QwTxtLayoutOwner *)context;
    if (!owner) return;
    free(owner->html);
    free(owner);
}

int qw_txt_format_open_layout(const void *data,
                              size_t size,
                              QwInputFormat input_format,
                              const QwConvertOptions *options,
                              QwLayoutDocument *out_document,
                              QwConvertReport *report)
{
    QwHtmlBuffer html = {0};
    const uint8_t *text_data = (const uint8_t *)data;
    size_t text_size = size;
    uint8_t *decoded_legacy = NULL;
    size_t decoded_legacy_size = 0;
    QwTxtLayoutOwner *owner;
    int used_gbk = 0;
    int rc;
    (void)options;
    (void)input_format;
    if (!out_document || !txt_probe(data, size)) return QW_STATUS_ERR_FORMAT;
    memset(out_document, 0, sizeof(*out_document));
    if (!(size >= 2u && ((text_data[0] == 0xffu && text_data[1] == 0xfeu) ||
                         (text_data[0] == 0xfeu && text_data[1] == 0xffu))) &&
        !txt_is_valid_utf8(text_data, text_size)) {
        rc = txt_decode_gbk(text_data, text_size,
                            &decoded_legacy, &decoded_legacy_size);
        if (rc != QW_STATUS_OK) {
            if (report) snprintf(report->note, sizeof(report->note),
                                 "text is neither valid UTF-8 nor decodable as GBK/CP936");
            return rc;
        }
        text_data = decoded_legacy;
        text_size = decoded_legacy_size;
        used_gbk = 1;
    }
    if (!text_to_html(&html, text_data, text_size)) {
        free(decoded_legacy);
        free(html.data);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    owner = (QwTxtLayoutOwner *)calloc(1u, sizeof(*owner));
    if (!owner) {
        free(decoded_legacy);
        free(html.data);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    owner->html = html.data;
    out_document->html_utf8 = (const uint8_t *)owner->html;
    out_document->html_size = html.size;
    out_document->context = owner;
    out_document->dispose = txt_layout_dispose;
    free(decoded_legacy);
    if (used_gbk && report) {
        snprintf(report->note, sizeof(report->note),
                 "BOM-less input was decoded as GBK/CP936 and reflowed into PDF");
    }
    return QW_STATUS_OK;
}



#ifndef QUIREWISE_STANDALONE_PLAIN_TEXT_PARSER
static int txt_convert_to_pdf(const void *data,
                              size_t size,
                              const QwConvertOptions *options,
                              uint8_t **out_pdf,
                              size_t *out_size,
                              QwConvertReport *report)
{
    QwLayoutDocument document;
    int rc;
    memset(&document, 0, sizeof(document));
    rc = qw_txt_format_open_layout(data, size, QW_FORMAT_TXT, options,
                                   &document, report);
    if (rc == QW_STATUS_OK) {
        /* Layout is available, but PDF emission belongs to the internal
         * Quirewise layout/PDF renderer, which this package does not provide. */
        rc = QW_STATUS_ERR_UNSUPPORTED;
    }
    qw_layout_document_dispose(&document);
    return rc;
}

static int textz_extract_document(const void *data, size_t size,
                                  QwTextArchiveKind kind,
                                  uint8_t **out_data, size_t *out_size)
{
    QwTextArchiveDocument document = {0};
    QwTextArchiveStatus status;
    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0;
    status = qw_text_archive_extract_primary(data, size, kind, &document, NULL);
    if (status != QW_TEXT_ARCHIVE_OK) {
        if (status == QW_TEXT_ARCHIVE_ERR_INVALID_ARGUMENT)
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        if (status == QW_TEXT_ARCHIVE_ERR_OUT_OF_MEMORY)
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        if (status == QW_TEXT_ARCHIVE_ERR_RESOURCE_LIMIT)
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        return QW_STATUS_ERR_FORMAT;
    }
    *out_data = document.text_bytes;
    *out_size = document.text_size;
    document.text_bytes = NULL;
    qw_text_archive_document_free(&document);
    return QW_STATUS_OK;
}

static int txtz_extract_document(const void *data, size_t size,
                                 uint8_t **out_data, size_t *out_size)
{
    return textz_extract_document(data, size, QW_TEXT_ARCHIVE_TXTZ,
                                  out_data, out_size);
}

static int pmlz_extract_document(const void *data, size_t size,
                                 uint8_t **out_data, size_t *out_size)
{
    return textz_extract_document(data, size, QW_TEXT_ARCHIVE_PMLZ,
                                  out_data, out_size);
}

static int txtz_normalize_text(const uint8_t *data, size_t size,
                               uint8_t **out_data, size_t *out_size)
{
    size_t offset = 0;
    *out_data = NULL;
    *out_size = 0;
    if (size >= 2u && ((data[0] == 0xffu && data[1] == 0xfeu) ||
                       (data[0] == 0xfeu && data[1] == 0xffu))) {
        QwHtmlBuffer utf8 = {0};
        int big_endian = data[0] == 0xfeu;
        offset = 2u;
        while (offset + 1u < size) {
            unsigned long cp = read_u16(data + offset, big_endian);
            offset += 2u;
            if (cp >= 0xd800u && cp <= 0xdbffu) {
                if (offset + 1u < size) {
                    unsigned long low = read_u16(data + offset, big_endian);
                    if (low >= 0xdc00u && low <= 0xdfffu) {
                        cp = 0x10000u + ((cp - 0xd800u) << 10) +
                             low - 0xdc00u;
                        offset += 2u;
                    } else {
                        cp = 0xfffdu;
                    }
                } else {
                    cp = 0xfffdu;
                }
            } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
                cp = 0xfffdu;
            }
            if (!html_append_codepoint(&utf8, cp)) {
                free(utf8.data);
                return QW_STATUS_ERR_OUT_OF_MEMORY;
            }
        }
        if (offset < size && !html_append_codepoint(&utf8, 0xfffdu)) {
            free(utf8.data);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
        *out_data = (uint8_t *)utf8.data;
        *out_size = utf8.size;
        return QW_STATUS_OK;
    }
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu) {
        offset = 3u;
    }
    data += offset;
    size -= offset;
    if (txt_is_valid_utf8(data, size)) {
        uint8_t *copy = (uint8_t *)malloc(size ? size : 1u);
        if (!copy) return QW_STATUS_ERR_OUT_OF_MEMORY;
        if (size) memcpy(copy, data, size);
        *out_data = copy;
        *out_size = size;
        return QW_STATUS_OK;
    }
    return txt_decode_gbk(data, size, out_data, out_size);
}

static int txtz_open_layout(const void *data, size_t size,
                            QwInputFormat input_format,
                            const QwConvertOptions *options,
                            QwLayoutDocument *out_document,
                            QwConvertReport *report)
{
    uint8_t *text = NULL;
    uint8_t *utf8 = NULL;
    size_t text_size = 0u;
    size_t utf8_size = 0u;
    int rc;
    if (!data || !size || !out_document || input_format != QW_FORMAT_TXTZ) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    memset(out_document, 0, sizeof(*out_document));
    rc = txtz_extract_document(data, size, &text, &text_size);
    if (rc != QW_STATUS_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "TXTz package has no supported plain-text entry");
        return rc;
    }
    rc = txtz_normalize_text(text, text_size, &utf8, &utf8_size);
    free(text);
    if (rc == QW_STATUS_OK) {
        rc = qw_txt_format_open_layout(utf8, utf8_size, QW_FORMAT_TXT,
                                       options, out_document, report);
    }
    free(utf8);
    if (rc == QW_STATUS_OK && report && report->note[0] == '\0') {
        snprintf(report->note, sizeof(report->note),
                 "TXTz text was extracted and normalized through the shared TXT layout reader; other archive entries and metadata are omitted");
    }
    return rc;
}

static int txtz_convert_to_pdf(const void *data, size_t size,
                               const QwConvertOptions *options,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    uint8_t *text = NULL;
    uint8_t *utf8 = NULL;
    size_t text_size = 0;
    size_t utf8_size = 0;
    int rc = txtz_extract_document(data, size, &text, &text_size);
    if (rc != QW_STATUS_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "TXTz package has no supported plain-text entry");
        return rc;
    }
    (void)options;
    rc = txtz_normalize_text(text, text_size, &utf8, &utf8_size);
    if (rc == QW_STATUS_OK) {
        rc = qw_native_text_pdf_from_utf8(utf8, utf8_size, out_pdf, out_size);
    }
    free(utf8);
    free(text);
    if (rc == QW_STATUS_OK && report && report->note[0] == '\0') {
        snprintf(report->note, sizeof(report->note),
                 "TXTz entry was extracted by the native ZIP reader and written by the native C text PDF route; characters outside WinAnsi appear as '?'");
    }
    return rc;
}

static int pmlz_convert_to_pdf(const void *data, size_t size,
                               const QwConvertOptions *options,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    uint8_t *pml = NULL;
    uint8_t *utf8 = NULL;
    size_t pml_size = 0;
    size_t utf8_size = 0;
    QwPmlDocument parsed = {0};
    QwPmlStatus pml_status;
    int rc;
    (void)options;
    rc = pmlz_extract_document(data, size, &pml, &pml_size);
    if (rc != QW_STATUS_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "PMLz package has no supported PML text entry");
        return rc;
    }
    pml_status = qw_pml_parse(pml, pml_size, &parsed);
    free(pml);
    if (pml_status == QW_PML_ERR_OUT_OF_MEMORY) rc = QW_STATUS_ERR_OUT_OF_MEMORY;
    else if (pml_status == QW_PML_ERR_RESOURCE_LIMIT) rc = QW_STATUS_ERR_RESOURCE_LIMIT;
    else if (pml_status == QW_PML_ERR_INVALID_ARGUMENT) rc = QW_STATUS_ERR_INVALID_ARGUMENT;
    else rc = QW_STATUS_OK;
    if (rc == QW_STATUS_OK)
        rc = txtz_normalize_text(parsed.text_bytes, parsed.text_size,
                                 &utf8, &utf8_size);
    qw_pml_document_free(&parsed);
    if (rc == QW_STATUS_OK && utf8_size == 0u) rc = QW_STATUS_ERR_EMPTY;
    if (rc == QW_STATUS_OK)
        rc = qw_native_text_pdf_from_utf8(utf8, utf8_size, out_pdf, out_size);
    free(utf8);
    if (rc == QW_STATUS_OK && report && report->note[0] == '\0') {
        snprintf(report->note, sizeof(report->note),
                 "PMLz text was extracted with basic paragraph markers; formatting and images are omitted, non-WinAnsi characters appear as '?'");
    }
    return rc;
}
















const QwFormatDescriptor qw_format_txt = {
    .id = QW_FORMAT_TXT,
    .name = "plain text",
    .capabilities = (QW_FORMAT_CAP_MULTI_PAGE) | (0u),
    .probe = txt_probe,
    .open_memory = NULL,
    .open_layout = qw_txt_format_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_TEXT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = txt_convert_to_pdf
};

const QwFormatDescriptor qw_format_txtz = {
    .id = QW_FORMAT_TXTZ,
    .name = "TXTz",
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE |
                    QW_FORMAT_CAP_ARCHIVE_PROBE,
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = txtz_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_TEXT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_TEXT_ARCHIVE,
    .convert_to_pdf = txtz_convert_to_pdf
};

const QwFormatDescriptor qw_format_pmlz = {
    .id = QW_FORMAT_PMLZ,
    .name = "PMLz",
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE |
                    QW_FORMAT_CAP_ARCHIVE_PROBE,
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_TEXT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_TEXT_ARCHIVE,
    .convert_to_pdf = pmlz_convert_to_pdf
};
#endif

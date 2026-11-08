#include "format_registry.h"
#include "../../core/byte_utils.h"
#include "txt_format.h"
#include <quirewise/charset.h>


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




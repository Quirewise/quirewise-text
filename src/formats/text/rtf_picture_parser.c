#include "rtf_picture_parser.h"

#include "quirewise/convert.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    RTF_PICTURE_MAX_BYTES = 32 * 1024 * 1024,
    RTF_PICTURE_GROUP_LIMIT = 64
};

typedef enum RtfPictureFormat {
    RTF_PICTURE_FORMAT_NONE,
    RTF_PICTURE_FORMAT_PNG,
    RTF_PICTURE_FORMAT_JPEG
} RtfPictureFormat;

typedef struct RtfPictureBytes {
    uint8_t *data;
    size_t size;
    size_t capacity;
} RtfPictureBytes;

static int rtf_picture_reserve(RtfPictureBytes *bytes, size_t additional)
{
    size_t required;
    size_t capacity;
    uint8_t *grown;
    if (additional > RTF_PICTURE_MAX_BYTES - bytes->size)
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    required = bytes->size + additional;
    if (required <= bytes->capacity) return QW_STATUS_OK;
    capacity = bytes->capacity ? bytes->capacity : 4096u;
    while (capacity < required) {
        if (capacity > RTF_PICTURE_MAX_BYTES / 2u) {
            capacity = RTF_PICTURE_MAX_BYTES;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < required) return QW_STATUS_ERR_RESOURCE_LIMIT;
    grown = (uint8_t *)realloc(bytes->data, capacity);
    if (!grown) return QW_STATUS_ERR_OUT_OF_MEMORY;
    bytes->data = grown;
    bytes->capacity = capacity;
    return QW_STATUS_OK;
}

static int rtf_picture_hex_value(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int rtf_picture_word_char(uint8_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

int qw_rtf_picture_group_probe(const uint8_t *data, size_t size)
{
    static const uint8_t prefix[] = {'{', '\\', 'p', 'i', 'c', 't'};
    uint8_t boundary;
    if (!data || size < sizeof(prefix) ||
        memcmp(data, prefix, sizeof(prefix)) != 0) return 0;
    if (size == sizeof(prefix)) return 0;
    boundary = data[sizeof(prefix)];
    return boundary == '\\' || boundary == ' ' || boundary == '\t' ||
           boundary == '\r' || boundary == '\n' || boundary == '}';
}

static int rtf_picture_append_byte(RtfPictureBytes *bytes, uint8_t value)
{
    int rc = rtf_picture_reserve(bytes, 1u);
    if (rc != QW_STATUS_OK) return rc;
    bytes->data[bytes->size++] = value;
    return QW_STATUS_OK;
}

static int rtf_picture_append_payload(RtfPictureBytes *bytes,
                                      const uint8_t *data,
                                      size_t size)
{
    int rc = rtf_picture_reserve(bytes, size);
    if (rc != QW_STATUS_OK) return rc;
    if (size) memcpy(bytes->data + bytes->size, data, size);
    bytes->size += size;
    return QW_STATUS_OK;
}

static int rtf_picture_append_html(QwTextBuffer *html,
                                  const RtfPictureBytes *bytes,
                                  RtfPictureFormat format)
{
    static const char png_prefix[] =
        "<p style=\"text-align:center\"><img alt=\"\" "
        "style=\"max-width:100%;height:auto\" src=\"data:image/png;base64,";
    static const char jpeg_prefix[] =
        "<p style=\"text-align:center\"><img alt=\"\" "
        "style=\"max-width:100%;height:auto\" src=\"data:image/jpeg;base64,";
    static const char suffix[] = "\"/></p>";
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *prefix = format == RTF_PICTURE_FORMAT_PNG
        ? png_prefix : jpeg_prefix;
    size_t prefix_size = strlen(prefix);
    size_t suffix_size = sizeof(suffix) - 1u;
    size_t encoded_size;
    size_t content_size;
    size_t position = 0u;

    if (bytes->size > SIZE_MAX - 2u) return QW_STATUS_ERR_RESOURCE_LIMIT;
    encoded_size = (bytes->size + 2u) / 3u;
    if (encoded_size > SIZE_MAX / 4u) return QW_STATUS_ERR_RESOURCE_LIMIT;
    encoded_size *= 4u;
    if (prefix_size > SIZE_MAX - encoded_size ||
        prefix_size + encoded_size > SIZE_MAX - suffix_size)
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    content_size = prefix_size + encoded_size + suffix_size;
    if (!html || html->size >= html->limit ||
        content_size > html->limit - html->size - 1u)
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (!qw_text_buffer_append(html, prefix, prefix_size))
        return QW_STATUS_ERR_OUT_OF_MEMORY;

    while (position < bytes->size) {
        char encoded[4096];
        size_t encoded_used = 0u;
        while (position < bytes->size && encoded_used + 4u <= sizeof(encoded)) {
            size_t remaining = bytes->size - position;
            uint32_t a = bytes->data[position++];
            uint32_t b = remaining > 1u ? bytes->data[position++] : 0u;
            uint32_t c = remaining > 2u ? bytes->data[position++] : 0u;
            encoded[encoded_used++] = alphabet[(a >> 2) & 0x3fu];
            encoded[encoded_used++] = alphabet[((a & 0x03u) << 4) |
                                                ((b >> 4) & 0x0fu)];
            encoded[encoded_used++] = remaining > 1u
                ? alphabet[((b & 0x0fu) << 2) | ((c >> 6) & 0x03u)] : '=';
            encoded[encoded_used++] = remaining > 2u
                ? alphabet[c & 0x3fu] : '=';
        }
        if (!qw_text_buffer_append(html, encoded, encoded_used))
            return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (!qw_text_buffer_append(html, suffix, suffix_size))
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    return QW_STATUS_OK;
}

static int rtf_picture_signature_matches(const RtfPictureBytes *bytes,
                                         RtfPictureFormat format)
{
    static const uint8_t png_signature[] = {
        0x89u, 'P', 'N', 'G', 0x0du, 0x0au, 0x1au, 0x0au
    };
    if (format == RTF_PICTURE_FORMAT_PNG) {
        return bytes->size >= sizeof(png_signature) &&
               memcmp(bytes->data, png_signature, sizeof(png_signature)) == 0;
    }
    return format == RTF_PICTURE_FORMAT_JPEG && bytes->size >= 3u &&
           bytes->data[0] == 0xffu && bytes->data[1] == 0xd8u &&
           bytes->data[2] == 0xffu;
}

int qw_rtf_picture_group_parse(const uint8_t *data,
                               size_t size,
                               size_t *consumed,
                               QwTextBuffer *html,
                               int *rendered)
{
    RtfPictureBytes bytes = {0};
    RtfPictureFormat format = RTF_PICTURE_FORMAT_NONE;
    size_t position = 0u;
    size_t depth = 0u;
    int high_nibble = -1;
    int invalid_payload = 0;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!data || !consumed || !html || !rendered)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *consumed = 0u;
    *rendered = 0;
    if (!qw_rtf_picture_group_probe(data, size))
        return QW_STATUS_ERR_UNSUPPORTED;

    while (position < size) {
        uint8_t ch = data[position++];
        if (ch == '{') {
            if (++depth > RTF_PICTURE_GROUP_LIMIT) {
                rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto cleanup;
            }
        } else if (ch == '}') {
            if (!depth) goto cleanup;
            if (--depth == 0u) break;
        } else if (ch == '\\') {
            uint8_t control;
            if (position >= size) goto cleanup;
            control = data[position++];
            if (control == '\'' ) {
                int high;
                int low;
                if (size - position < 2u ||
                    (high = rtf_picture_hex_value(data[position])) < 0 ||
                    (low = rtf_picture_hex_value(data[position + 1u])) < 0)
                    goto cleanup;
                position += 2u;
                if (depth == 1u && format != RTF_PICTURE_FORMAT_NONE) {
                    if (high_nibble >= 0) invalid_payload = 1;
                    else {
                        rc = rtf_picture_append_byte(&bytes,
                            (uint8_t)((high << 4) | low));
                        if (rc != QW_STATUS_OK) goto cleanup;
                    }
                }
            } else if (rtf_picture_word_char(control)) {
                char word[32];
                size_t word_size = 0u;
                size_t parameter = 0u;
                int has_parameter = 0;
                int negative = 0;
                int parameter_overflow = 0;
                word[word_size++] = (char)control;
                while (position < size && rtf_picture_word_char(data[position])) {
                    if (word_size + 1u < sizeof(word))
                        word[word_size++] = (char)data[position];
                    ++position;
                }
                word[word_size] = '\0';
                if (position < size &&
                    (data[position] == '-' ||
                     (data[position] >= '0' && data[position] <= '9'))) {
                    has_parameter = 1;
                    if (data[position] == '-') {
                        negative = 1;
                        ++position;
                    }
                    while (position < size && data[position] >= '0' &&
                           data[position] <= '9') {
                        size_t digit = (size_t)(data[position] - '0');
                        if (parameter > (SIZE_MAX - digit) / 10u)
                            parameter_overflow = 1;
                        else if (!parameter_overflow)
                            parameter = parameter * 10u + digit;
                        ++position;
                    }
                    if (parameter_overflow) goto cleanup;
                }
                if (position < size && data[position] == ' ') ++position;
                if (depth == 1u) {
                    if (strcmp(word, "pngblip") == 0)
                        format = RTF_PICTURE_FORMAT_PNG;
                    else if (strcmp(word, "jpegblip") == 0)
                        format = RTF_PICTURE_FORMAT_JPEG;
                }
                if (strcmp(word, "bin") == 0) {
                    if (!has_parameter || negative || parameter > size - position)
                        goto cleanup;
                    if (depth == 1u && format != RTF_PICTURE_FORMAT_NONE) {
                        if (high_nibble >= 0) invalid_payload = 1;
                        rc = rtf_picture_append_payload(&bytes, data + position,
                                                       parameter);
                        if (rc != QW_STATUS_OK) goto cleanup;
                    }
                    position += parameter;
                }
            } else if (control == '\\' || control == '{' || control == '}') {
                if (depth == 1u && format != RTF_PICTURE_FORMAT_NONE)
                    invalid_payload = 1;
            }
        } else if (depth == 1u && format != RTF_PICTURE_FORMAT_NONE) {
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
            {
                int nibble = rtf_picture_hex_value(ch);
                if (nibble < 0) {
                    invalid_payload = 1;
                } else if (high_nibble < 0) {
                    high_nibble = nibble;
                } else {
                    rc = rtf_picture_append_byte(&bytes,
                        (uint8_t)((high_nibble << 4) | nibble));
                    if (rc != QW_STATUS_OK) goto cleanup;
                    high_nibble = -1;
                }
            }
        }
    }

    if (depth != 0u) goto cleanup;
    *consumed = position;
    if (format != RTF_PICTURE_FORMAT_NONE && !invalid_payload &&
        high_nibble < 0 && rtf_picture_signature_matches(&bytes, format)) {
        rc = rtf_picture_append_html(html, &bytes, format);
        if (rc != QW_STATUS_OK) goto cleanup;
        *rendered = 1;
    }
    rc = QW_STATUS_OK;

cleanup:
    free(bytes.data);
    return rc;
}

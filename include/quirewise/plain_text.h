#ifndef QUIREWISE_PLAIN_TEXT_H
#define QUIREWISE_PLAIN_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwPlainTextStatus {
    QW_PLAIN_TEXT_OK = 0,
    QW_PLAIN_TEXT_ERROR_INVALID_ARGUMENT,
    QW_PLAIN_TEXT_ERROR_OUT_OF_MEMORY,
    QW_PLAIN_TEXT_ERROR_FORMAT,
    QW_PLAIN_TEXT_ERROR_UNSUPPORTED,
    QW_PLAIN_TEXT_ERROR_DECODE,
    QW_PLAIN_TEXT_ERROR_RESOURCE_LIMIT,
    QW_PLAIN_TEXT_ERROR_UNKNOWN
} QwPlainTextStatus;

typedef struct QwPlainTextDocument QwPlainTextDocument;

int qw_plain_text_probe(const void *data, size_t size);

QwPlainTextStatus qw_plain_text_parse(const void *data, size_t size,
                                      QwPlainTextDocument **out_document);

/* Returns the UTF-8 HTML representation; bytes remain valid until free. */
const uint8_t *qw_plain_text_document_html(
    const QwPlainTextDocument *document, size_t *out_size);

void qw_plain_text_document_free(QwPlainTextDocument *document);

#ifdef __cplusplus
}
#endif

#endif

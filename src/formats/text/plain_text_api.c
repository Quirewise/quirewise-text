#include "quirewise/plain_text.h"

#include "txt_format.h"
#include "quirewise/status.h"

#include <stdlib.h>

struct QwPlainTextDocument {
    QwLayoutDocument layout;
};

static QwPlainTextStatus plain_text_status_from_parser(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_PLAIN_TEXT_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT:
        return QW_PLAIN_TEXT_ERROR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_PLAIN_TEXT_ERROR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_FORMAT: return QW_PLAIN_TEXT_ERROR_FORMAT;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_PLAIN_TEXT_ERROR_UNSUPPORTED;
    case QW_STATUS_ERR_DECODE: return QW_PLAIN_TEXT_ERROR_DECODE;
    case QW_STATUS_ERR_RESOURCE_LIMIT:
        return QW_PLAIN_TEXT_ERROR_RESOURCE_LIMIT;
    default: return QW_PLAIN_TEXT_ERROR_UNKNOWN;
    }
}

int qw_plain_text_probe(const void *data, size_t size)
{
    return size <= QW_FORMAT_INPUT_LIMIT_TEXT &&
           qw_txt_native_probe(data, size);
}

QwPlainTextStatus qw_plain_text_parse(const void *data, size_t size,
                                      QwPlainTextDocument **out_document)
{
    QwPlainTextDocument *document;
    int status;
    if (!out_document) return QW_PLAIN_TEXT_ERROR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (!data || !size) return QW_PLAIN_TEXT_ERROR_INVALID_ARGUMENT;
    if (size > QW_FORMAT_INPUT_LIMIT_TEXT)
        return QW_PLAIN_TEXT_ERROR_RESOURCE_LIMIT;
    document = (QwPlainTextDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_PLAIN_TEXT_ERROR_OUT_OF_MEMORY;
    status = qw_txt_format_open_layout(data, size, QW_FORMAT_TXT, NULL,
                                       &document->layout, NULL);
    if (status != QW_STATUS_OK) {
        if (document->layout.dispose)
            document->layout.dispose(document->layout.context);
        free(document);
        return plain_text_status_from_parser(status);
    }
    *out_document = document;
    return QW_PLAIN_TEXT_OK;
}

const uint8_t *qw_plain_text_document_html(
    const QwPlainTextDocument *document, size_t *out_size)
{
    if (out_size) *out_size = 0u;
    if (!document || !out_size) return NULL;
    *out_size = document->layout.html_size;
    return document->layout.html_utf8;
}

void qw_plain_text_document_free(QwPlainTextDocument *document)
{
    if (!document) return;
    if (document->layout.dispose)
        document->layout.dispose(document->layout.context);
    free(document);
}

#include "quirewise/markdown.h"

#include "markdown_parser.h"
#include "quirewise/status.h"

#include <stdlib.h>
#include <string.h>

int qw_markdown_abi_version(void)
{
    return QW_MARKDOWN_ABI_VERSION;
}

static QwMarkdownStatus qw_markdown_map_status(int status)
{
    switch (status) {
    case QW_STATUS_OK:
        return QW_MARKDOWN_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT:
        return QW_MARKDOWN_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY:
        return QW_MARKDOWN_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_UNSUPPORTED:
        return QW_MARKDOWN_ERR_UNSUPPORTED;
    case QW_STATUS_ERR_RESOURCE_LIMIT:
        return QW_MARKDOWN_ERR_RESOURCE_LIMIT;
    default:
        return QW_MARKDOWN_ERR_FORMAT;
    }
}

QwMarkdownStatus qw_markdown_render_html(const void *data, size_t size,
                                        QwMarkdownDocument *out_document)
{
    QwTextBuffer html = {0};
    size_t removed_images = 0u;
    int rc;

    if (out_document) memset(out_document, 0, sizeof(*out_document));
    if (!data || !size || !out_document)
        return QW_MARKDOWN_ERR_INVALID_ARGUMENT;

    html.limit = (size_t)512u * 1024u * 1024u;
    rc = qw_markdown_parse_html((const uint8_t *)data, size, &html,
                                &removed_images);
    if (rc != QW_STATUS_OK) {
        free(html.data);
        return qw_markdown_map_status(rc);
    }
    if (!html.data) {
        html.data = (char *)malloc(1u);
        if (!html.data) return QW_MARKDOWN_ERR_OUT_OF_MEMORY;
        html.data[0] = '\0';
    }

    out_document->html = html.data;
    out_document->html_size = html.size;
    out_document->removed_images = removed_images;
    return QW_MARKDOWN_OK;
}

void qw_markdown_document_free(QwMarkdownDocument *document)
{
    if (!document) return;
    free(document->html);
    memset(document, 0, sizeof(*document));
}

const char *qw_markdown_status_string(QwMarkdownStatus status)
{
    switch (status) {
    case QW_MARKDOWN_OK:
        return "success";
    case QW_MARKDOWN_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case QW_MARKDOWN_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case QW_MARKDOWN_ERR_FORMAT:
        return "invalid Markdown";
    case QW_MARKDOWN_ERR_UNSUPPORTED:
        return "unsupported Markdown feature";
    case QW_MARKDOWN_ERR_RESOURCE_LIMIT:
        return "Markdown resource limit exceeded";
    default:
        return "unknown Markdown status";
    }
}

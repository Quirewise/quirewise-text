#include "markdown_format.h"

#include "markdown_parser.h"
#include "quirewise/markdown.h"
#include "quirewise/status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int markdown_status_to_quirewise(QwMarkdownStatus status)
{
    switch (status) {
    case QW_MARKDOWN_OK:
        return QW_STATUS_OK;
    case QW_MARKDOWN_ERR_INVALID_ARGUMENT:
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    case QW_MARKDOWN_ERR_OUT_OF_MEMORY:
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    case QW_MARKDOWN_ERR_UNSUPPORTED:
        return QW_STATUS_ERR_UNSUPPORTED;
    case QW_MARKDOWN_ERR_RESOURCE_LIMIT:
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    case QW_MARKDOWN_ERR_FORMAT:
    default:
        return QW_STATUS_ERR_FORMAT;
    }
}

static void markdown_layout_dispose(void *context)
{
    free(context);
}

int qw_markdown_format_open_layout(const void *data,
                                   size_t size,
                                   QwInputFormat input_format,
                                   const QwConvertOptions *options,
                                   QwLayoutDocument *out_document,
                                   QwConvertReport *report)
{
    QwMarkdownDocument rendered = {0};
    QwMarkdownStatus status;
    int rc;

    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_MARKDOWN)
        return QW_STATUS_ERR_INVALID_ARGUMENT;

    status = qw_markdown_render_html(data, size, &rendered);
    rc = markdown_status_to_quirewise(status);
    if (rc != QW_STATUS_OK) {
        qw_markdown_document_free(&rendered);
        return rc;
    }

    out_document->html_utf8 = (const uint8_t *)rendered.html;
    out_document->html_size = rendered.html_size;
    out_document->context = rendered.html;
    out_document->dispose = markdown_layout_dispose;
    if (report) {
        report->format = input_format;
        if (rendered.removed_images) {
            (void)snprintf(report->note, sizeof(report->note),
                           "%zu Markdown image(s) were replaced with alt text",
                           rendered.removed_images);
        }
    }
    return QW_STATUS_OK;
}

const QwFormatDescriptor qw_format_markdown = {
    .id = QW_FORMAT_MARKDOWN,
    .name = "Markdown",
    .probe = qw_markdown_probe,
    .open_layout = qw_markdown_format_open_layout,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_TEXT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};

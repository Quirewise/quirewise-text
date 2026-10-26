#include "quirewise/rtf.h"

#include "quirewise/status.h"
#include "rtf_parser.h"

#include <stdlib.h>
#include <string.h>

int qw_rtf_abi_version(void)
{
    return QW_RTF_ABI_VERSION;
}

static QwRtfStatus qw_rtf_map_status(int status)
{
    switch (status) {
    case QW_STATUS_OK:
        return QW_RTF_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT:
        return QW_RTF_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY:
        return QW_RTF_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_UNSUPPORTED:
        return QW_RTF_ERR_UNSUPPORTED;
    case QW_STATUS_ERR_RESOURCE_LIMIT:
        return QW_RTF_ERR_RESOURCE_LIMIT;
    case QW_STATUS_ERR_DECODE:
        return QW_RTF_ERR_ENCODING;
    default:
        return QW_RTF_ERR_FORMAT;
    }
}

QwRtfStatus qw_rtf_parse_html(const void *data, size_t size,
                              QwRtfDocument *out_document)
{
    QwTextBuffer html = {0};
    QwRtfParseSummary summary = {0};
    int rc;

    if (out_document) memset(out_document, 0, sizeof(*out_document));
    if (!data || !size || !out_document) return QW_RTF_ERR_INVALID_ARGUMENT;

    rc = qw_rtf_parse(data, size, &html, &summary);
    if (rc != QW_STATUS_OK) {
        free(html.data);
        return qw_rtf_map_status(rc);
    }
    if (!html.data) {
        html.data = (char *)malloc(1u);
        if (!html.data) return QW_RTF_ERR_OUT_OF_MEMORY;
        html.data[0] = '\0';
    }

    out_document->html = html.data;
    out_document->html_size = html.size;
    out_document->saw_table = summary.saw_table;
    out_document->saw_picture = summary.saw_picture;
    out_document->rendered_picture = summary.rendered_picture;
    out_document->omitted_picture = summary.omitted_picture;
    out_document->saw_object = summary.saw_object;
    out_document->saw_header_footer = summary.saw_header_footer;
    out_document->saw_footnote = summary.saw_footnote;
    return QW_RTF_OK;
}

void qw_rtf_document_free(QwRtfDocument *document)
{
    if (!document) return;
    free(document->html);
    memset(document, 0, sizeof(*document));
}

const char *qw_rtf_status_string(QwRtfStatus status)
{
    switch (status) {
    case QW_RTF_OK:
        return "success";
    case QW_RTF_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case QW_RTF_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case QW_RTF_ERR_FORMAT:
        return "invalid RTF";
    case QW_RTF_ERR_UNSUPPORTED:
        return "unsupported RTF feature";
    case QW_RTF_ERR_RESOURCE_LIMIT:
        return "RTF resource limit exceeded";
    case QW_RTF_ERR_ENCODING:
        return "RTF character encoding error";
    default:
        return "unknown RTF status";
    }
}

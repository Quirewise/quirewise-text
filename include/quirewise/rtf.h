#ifndef QUIREWISE_RTF_H
#define QUIREWISE_RTF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_RTF_ABI_VERSION 1

typedef enum QwRtfStatus {
    QW_RTF_OK = 0,
    QW_RTF_ERR_INVALID_ARGUMENT = 1,
    QW_RTF_ERR_OUT_OF_MEMORY = 2,
    QW_RTF_ERR_FORMAT = 3,
    QW_RTF_ERR_UNSUPPORTED = 4,
    QW_RTF_ERR_RESOURCE_LIMIT = 5,
    QW_RTF_ERR_ENCODING = 6
} QwRtfStatus;

typedef struct QwRtfDocument {
    char *html; /* NUL-terminated UTF-8 HTML fragment, owned by this structure */
    size_t html_size; /* Byte count excluding the trailing NUL */
    int saw_table;
    int saw_picture;
    int rendered_picture;
    int omitted_picture;
    int saw_object;
    int saw_header_footer;
    int saw_footnote;
} QwRtfDocument;

int qw_rtf_abi_version(void);

/* Parse supported RTF into a UTF-8 HTML fragment. Input bytes are borrowed. */
QwRtfStatus qw_rtf_parse_html(const void *data, size_t size,
                              QwRtfDocument *out_document);
void qw_rtf_document_free(QwRtfDocument *document);
const char *qw_rtf_status_string(QwRtfStatus status);

#ifdef __cplusplus
}
#endif

#endif

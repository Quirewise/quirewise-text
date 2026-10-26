#ifndef QUIREWISE_MAN_H
#define QUIREWISE_MAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_MAN_ABI_VERSION 1

typedef enum QwManStatus {
    QW_MAN_OK = 0,
    QW_MAN_ERR_INVALID_ARGUMENT = 1,
    QW_MAN_ERR_INVALID_UTF8 = 2,
    QW_MAN_ERR_OUT_OF_MEMORY = 3,
    QW_MAN_ERR_RESOURCE_LIMIT = 4,
    QW_MAN_ERR_FORMAT = 5
} QwManStatus;

typedef struct QwManDocument {
    char *html_utf8; /* NUL-terminated UTF-8 HTML, owned by this structure */
    size_t html_size; /* Byte count excluding the trailing NUL */
    int skipped_unsupported_macros; /* Nonzero if unsupported roff macros were skipped */
} QwManDocument;

int qw_man_abi_version(void);

/* Parse a UTF-8 man/roff document from the supported macro subset to HTML. */
QwManStatus qw_man_parse_html(const void *data, size_t size,
                              QwManDocument *out_document);
void qw_man_document_free(QwManDocument *document);
const char *qw_man_status_string(QwManStatus status);

#ifdef __cplusplus
}
#endif

#endif

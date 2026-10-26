#ifndef QUIREWISE_RST_H
#define QUIREWISE_RST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_RST_ABI_VERSION 1

typedef enum QwRstStatus {
    QW_RST_OK = 0,
    QW_RST_ERR_INVALID_ARGUMENT = 1,
    QW_RST_ERR_OUT_OF_MEMORY = 2,
    QW_RST_ERR_RESOURCE_LIMIT = 3
} QwRstStatus;

typedef struct QwRstDocument {
    char *markdown; /* NUL-terminated UTF-8 Markdown, owned by this structure */
    size_t markdown_size; /* Byte count excluding the trailing NUL */
    int ignored_directives;
    int omitted_images;
    int dropped_unsafe_links;
} QwRstDocument;

int qw_rst_abi_version(void);

/* Parse UTF-8 input from the supported reStructuredText subset into Markdown. */
QwRstStatus qw_rst_parse_markdown(const void *data, size_t size,
                                 QwRstDocument *out_document);
void qw_rst_document_free(QwRstDocument *document);
const char *qw_rst_status_string(QwRstStatus status);

#ifdef __cplusplus
}
#endif

#endif

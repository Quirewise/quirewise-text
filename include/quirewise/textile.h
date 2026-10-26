#ifndef QUIREWISE_TEXTILE_H
#define QUIREWISE_TEXTILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_TEXTILE_ABI_VERSION 1

typedef enum QwTextileStatus {
    QW_TEXTILE_OK = 0,
    QW_TEXTILE_ERR_INVALID_ARGUMENT = 1,
    QW_TEXTILE_ERR_OUT_OF_MEMORY = 2,
    QW_TEXTILE_ERR_RESOURCE_LIMIT = 3
} QwTextileStatus;

typedef struct QwTextileDocument {
    char *markdown; /* NUL-terminated UTF-8 Markdown, owned by this structure */
    size_t markdown_size; /* Byte count excluding the trailing NUL */
    int omitted_images;
    int dropped_unsafe_links;
} QwTextileDocument;

int qw_textile_abi_version(void);

/* Parse the supported Textile subset into Markdown. The input is borrowed. */
QwTextileStatus qw_textile_parse_markdown(const void *data, size_t size,
                                         QwTextileDocument *out_document);
void qw_textile_document_free(QwTextileDocument *document);
const char *qw_textile_status_string(QwTextileStatus status);

#ifdef __cplusplus
}
#endif

#endif

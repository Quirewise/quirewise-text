#ifndef QUIREWISE_ASCIIDOC_H
#define QUIREWISE_ASCIIDOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_ASCIIDOC_ABI_VERSION 1

typedef enum QwAsciiDocStatus {
    QW_ASCIIDOC_OK = 0,
    QW_ASCIIDOC_ERR_INVALID_ARGUMENT = 1,
    QW_ASCIIDOC_ERR_INVALID_UTF8 = 2,
    QW_ASCIIDOC_ERR_OUT_OF_MEMORY = 3,
    QW_ASCIIDOC_ERR_RESOURCE_LIMIT = 4
} QwAsciiDocStatus;

typedef struct QwAsciiDocDocument {
    char *markdown; /* NUL-terminated UTF-8 Markdown, owned by this structure */
    size_t markdown_size; /* Byte count excluding the trailing NUL */
    int omitted_images;
    int skipped_directives;
    int skipped_passthrough;
    int dropped_unsafe_links;
} QwAsciiDocDocument;

int qw_asciidoc_abi_version(void);

/* Parse UTF-8 input from the supported AsciiDoc subset into Markdown. */
QwAsciiDocStatus qw_asciidoc_parse_markdown(const void *data, size_t size,
                                           QwAsciiDocDocument *out_document);
void qw_asciidoc_document_free(QwAsciiDocDocument *document);
const char *qw_asciidoc_status_string(QwAsciiDocStatus status);

#ifdef __cplusplus
}
#endif

#endif

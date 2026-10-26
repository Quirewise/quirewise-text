#ifndef QUIREWISE_TYPST_H
#define QUIREWISE_TYPST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_TYPST_ABI_VERSION 1

typedef enum QwTypstStatus {
    QW_TYPST_OK = 0,
    QW_TYPST_ERR_INVALID_ARGUMENT = 1,
    QW_TYPST_ERR_INVALID_UTF8 = 2,
    QW_TYPST_ERR_OUT_OF_MEMORY = 3,
    QW_TYPST_ERR_RESOURCE_LIMIT = 4
} QwTypstStatus;

typedef struct QwTypstDocument {
    char *markdown; /* NUL-terminated UTF-8 Markdown, owned by this structure */
    size_t markdown_size; /* Byte count excluding the trailing NUL */
    int omitted_images;
    int skipped_code_expressions;
    int dropped_unsafe_links;
    int reduced_math;
} QwTypstDocument;

int qw_typst_abi_version(void);

/* Parse UTF-8 input from the supported Typst markup subset into Markdown. */
QwTypstStatus qw_typst_parse_markdown(const void *data, size_t size,
                                     QwTypstDocument *out_document);
void qw_typst_document_free(QwTypstDocument *document);
const char *qw_typst_status_string(QwTypstStatus status);

#ifdef __cplusplus
}
#endif

#endif

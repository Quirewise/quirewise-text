#ifndef QUIREWISE_ORG_H
#define QUIREWISE_ORG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_ORG_ABI_VERSION 1

typedef enum QwOrgStatus {
    QW_ORG_OK = 0,
    QW_ORG_ERR_INVALID_ARGUMENT = 1,
    QW_ORG_ERR_OUT_OF_MEMORY = 2,
    QW_ORG_ERR_RESOURCE_LIMIT = 3
} QwOrgStatus;

typedef struct QwOrgDocument {
    char *markdown; /* NUL-terminated UTF-8 Markdown, owned by this structure */
    size_t markdown_size; /* Byte count excluding the trailing NUL */
    int omitted_export_blocks;
    int dropped_unsafe_links;
} QwOrgDocument;

int qw_org_abi_version(void);

/* Parse UTF-8 input from the supported Org mode subset into Markdown. */
QwOrgStatus qw_org_parse_markdown(const void *data, size_t size,
                                  QwOrgDocument *out_document);
void qw_org_document_free(QwOrgDocument *document);
const char *qw_org_status_string(QwOrgStatus status);

#ifdef __cplusplus
}
#endif

#endif

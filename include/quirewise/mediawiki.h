#ifndef QUIREWISE_MEDIAWIKI_H
#define QUIREWISE_MEDIAWIKI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_MEDIAWIKI_ABI_VERSION 1

typedef enum QwMediaWikiStatus {
    QW_MEDIAWIKI_OK = 0,
    QW_MEDIAWIKI_ERR_INVALID_ARGUMENT = 1,
    QW_MEDIAWIKI_ERR_OUT_OF_MEMORY = 2,
    QW_MEDIAWIKI_ERR_RESOURCE_LIMIT = 3
} QwMediaWikiStatus;

typedef struct QwMediaWikiDocument {
    char *markdown; /* NUL-terminated UTF-8 Markdown, owned by this structure */
    size_t markdown_size; /* Byte count excluding the trailing NUL */
    int omitted_media;
    int dropped_unsafe_links;
} QwMediaWikiDocument;

int qw_mediawiki_abi_version(void);

/* Parse UTF-8 input from the supported MediaWiki subset into Markdown. */
QwMediaWikiStatus qw_mediawiki_parse_markdown(const void *data, size_t size,
                                             QwMediaWikiDocument *out_document);
void qw_mediawiki_document_free(QwMediaWikiDocument *document);
const char *qw_mediawiki_status_string(QwMediaWikiStatus status);

#ifdef __cplusplus
}
#endif

#endif

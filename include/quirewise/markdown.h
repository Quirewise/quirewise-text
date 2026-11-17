#ifndef QUIREWISE_MARKDOWN_H
#define QUIREWISE_MARKDOWN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_MARKDOWN_ABI_VERSION 1

typedef enum QwMarkdownStatus {
    QW_MARKDOWN_OK = 0,
    QW_MARKDOWN_ERR_INVALID_ARGUMENT = 1,
    QW_MARKDOWN_ERR_OUT_OF_MEMORY = 2,
    QW_MARKDOWN_ERR_FORMAT = 3,
    QW_MARKDOWN_ERR_UNSUPPORTED = 4,
    QW_MARKDOWN_ERR_RESOURCE_LIMIT = 5
} QwMarkdownStatus;

typedef struct QwMarkdownDocument {
    char *html; /* NUL-terminated UTF-8 HTML fragment, owned by this structure */
    size_t html_size; /* Byte count excluding the trailing NUL */
    size_t removed_images; /* Images are replaced by their alt text */
} QwMarkdownDocument;

typedef struct QwMarkdownText {
    char *markdown; /* NUL-terminated UTF-8 Markdown, owned by this structure */
    size_t markdown_size; /* Byte count excluding the trailing NUL */
    size_t removed_images; /* Images are replaced by their alt text */
} QwMarkdownText;

/*! Return the ABI version compiled into the installed text package. */
int qw_markdown_abi_version(void);

/* Render the supported Markdown subset to an HTML fragment. Input is borrowed. */
QwMarkdownStatus qw_markdown_render_html(const void *data, size_t size,
                                        QwMarkdownDocument *out_document);
/*! Release a document returned by the parser; NULL is accepted. */
void qw_markdown_document_free(QwMarkdownDocument *document);

/*
 * Convert an HTML fragment (headings, paragraphs, emphasis, links, lists,
 * blockquotes, inline/fenced code, simple tables, images) to the supported
 * Markdown subset. Script, style and head content is dropped; images become
 * their alt text. Input is borrowed; the result is owned by the caller and
 * released with qw_markdown_text_free().
 */
QwMarkdownStatus qw_markdown_from_html(const void *html, size_t size,
                                       QwMarkdownText *out_document);
/*! Release a Markdown result returned by qw_markdown_from_html(). */
void qw_markdown_text_free(QwMarkdownText *document);
/*! Return a static human-readable name for a status code. */
const char *qw_markdown_status_string(QwMarkdownStatus status);

#ifdef __cplusplus
}
#endif

#endif

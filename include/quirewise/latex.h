#ifndef QUIREWISE_LATEX_H
#define QUIREWISE_LATEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_LATEX_ABI_VERSION 1

typedef enum QwLatexStatus {
    QW_LATEX_OK = 0,
    QW_LATEX_ERR_INVALID_ARGUMENT = 1,
    QW_LATEX_ERR_INVALID_UTF8 = 2,
    QW_LATEX_ERR_OUT_OF_MEMORY = 3,
    QW_LATEX_ERR_RESOURCE_LIMIT = 4
} QwLatexStatus;

typedef struct QwLatexDocument {
    char *markdown; /* NUL-terminated UTF-8 Markdown, owned by this structure */
    size_t markdown_size; /* Byte count excluding the trailing NUL */
    char *title; /* Optional extracted title bytes, owned by this structure */
    size_t title_size;
    int skipped_commands;
    int omitted_images;
    int omitted_tables;
    int omitted_figures;
    int reduced_math;
    int dropped_unsafe_links;
} QwLatexDocument;

int qw_latex_abi_version(void);

/* Parse UTF-8 input from the supported LaTeX subset into Markdown. */
QwLatexStatus qw_latex_parse_markdown(const void *data, size_t size,
                                     QwLatexDocument *out_document);
void qw_latex_document_free(QwLatexDocument *document);
const char *qw_latex_status_string(QwLatexStatus status);

#ifdef __cplusplus
}
#endif

#endif

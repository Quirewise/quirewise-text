#ifndef QUIREWISE_HTML_GRID_H
#define QUIREWISE_HTML_GRID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwHtmlGridResult {
    QW_HTML_GRID_OK = 0,
    QW_HTML_GRID_ERROR_INVALID_ARGUMENT = -1,
    QW_HTML_GRID_ERROR_FORMAT = -4,
    QW_HTML_GRID_ERROR_UNSUPPORTED = -5,
    QW_HTML_GRID_ERROR_RESOURCE_LIMIT = -11
} QwHtmlGridResult;

/*
 * Input is expected to be UTF-8. Callbacks receive zero-based row and column
 * indices. Cell text and table names are UTF-8 byte spans borrowed only for
 * the duration of the callback. A callback may return its own nonzero status;
 * that value is propagated.
 */
typedef struct QwHtmlGridVisitor {
    int (*begin_row)(void *context, size_t row_index);
    int (*cell)(void *context, size_t row_index, size_t column_index,
                const uint8_t *text, size_t text_size);
    int (*end_row)(void *context, size_t row_index);
} QwHtmlGridVisitor;

typedef struct QwHtmlGridDocumentVisitor {
    int (*begin_table)(void *context, size_t table_index,
                       const uint8_t *name, size_t name_size);
    QwHtmlGridVisitor grid;
    int (*end_table)(void *context, size_t table_index);
} QwHtmlGridDocumentVisitor;

/* Parse the supported HTML-grid subset containing exactly one table. */
int qw_html_grid_visit_single_table(const uint8_t *html, size_t html_size,
                                    const QwHtmlGridVisitor *visitor,
                                    void *context);

/* Parse every supported table in an HTML-grid document. */
int qw_html_grid_visit_tables(const uint8_t *html, size_t html_size,
                              const QwHtmlGridDocumentVisitor *visitor,
                              void *context);

#ifdef __cplusplus
}
#endif

#endif

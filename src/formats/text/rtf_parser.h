#ifndef QUIREWISE_RTF_PARSER_H
#define QUIREWISE_RTF_PARSER_H

#include "../../core/text_buffer.h"

#include <stddef.h>
#include <stdint.h>

enum { QW_RTF_MAX_INPUT = 256 * 1024 * 1024 };

typedef struct QwRtfParseSummary {
    int saw_table;
    int saw_picture;
    int rendered_picture;
    int omitted_picture;
    int saw_object;
    int saw_header_footer;
    int saw_footnote;
} QwRtfParseSummary;

int qw_rtf_probe(const void *data, size_t size);
int qw_rtf_parse(const void *data,
                 size_t size,
                 QwTextBuffer *out_html,
                 QwRtfParseSummary *out_summary);

#endif

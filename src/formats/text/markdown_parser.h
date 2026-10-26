#ifndef QUIREWISE_MARKDOWN_PARSER_H
#define QUIREWISE_MARKDOWN_PARSER_H

#include "../../core/text_buffer.h"

#include <stddef.h>
#include <stdint.h>

int qw_markdown_probe(const void *source, size_t size);
int qw_markdown_parse_html(const uint8_t *source, size_t size,
                           QwTextBuffer *html_body,
                           size_t *out_removed_images);

#endif

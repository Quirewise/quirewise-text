#ifndef QUIREWISE_RTF_PICTURE_PARSER_H
#define QUIREWISE_RTF_PICTURE_PARSER_H

#include "../../core/text_buffer.h"

#include <stddef.h>
#include <stdint.h>

int qw_rtf_picture_group_probe(const uint8_t *data, size_t size);
int qw_rtf_picture_group_parse(const uint8_t *data,
                               size_t size,
                               size_t *consumed,
                               QwTextBuffer *html,
                               int *rendered);

#endif

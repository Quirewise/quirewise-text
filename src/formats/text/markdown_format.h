#ifndef QUIREWISE_MARKDOWN_FORMAT_INTERNAL_H
#define QUIREWISE_MARKDOWN_FORMAT_INTERNAL_H

#include "../../core/layout_document.h"
#include "../../core/format_registry.h"

extern const QwFormatDescriptor qw_format_markdown;

int qw_markdown_format_open_layout(const void *data,
                                   size_t size,
                                   QwInputFormat input_format,
                                   const QwConvertOptions *options,
                                   QwLayoutDocument *out_document,
                                   QwConvertReport *report);

#endif

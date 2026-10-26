#ifndef QUIREWISE_TXT_FORMAT_INTERNAL_H
#define QUIREWISE_TXT_FORMAT_INTERNAL_H

#include "../../core/layout_document.h"
#include "../../core/format_registry.h"

#ifndef QUIREWISE_STANDALONE_PLAIN_TEXT_PARSER
extern const QwFormatDescriptor qw_format_txt;
#endif

int qw_txt_format_open_layout(const void *data,
                              size_t size,
                              QwInputFormat input_format,
                              const QwConvertOptions *options,
                              QwLayoutDocument *out_document,
                              QwConvertReport *report);

#ifdef QUIREWISE_STANDALONE_PLAIN_TEXT_PARSER
int qw_txt_native_probe(const void *data, size_t size);
#endif

#endif

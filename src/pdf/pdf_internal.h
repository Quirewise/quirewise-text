#ifndef QUIREWISE_PDF_INTERNAL_H
#define QUIREWISE_PDF_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct PdfBuf {
    uint8_t *data;
    size_t size;
    size_t cap;
    FILE *stream;
    size_t max_stream_size;
    int limit_exceeded;
    int oom;
    int io_error;
} PdfBuf;

void pdf_buf_init(PdfBuf *b);
void pdf_buf_free(PdfBuf *b);
void pdf_buf_put(PdfBuf *b, const void *bytes, size_t len);
void pdf_buf_puts(PdfBuf *b, const char *text);
void pdf_buf_printf(PdfBuf *b, const char *fmt, ...);

typedef struct PdfWriter {
    PdfBuf buf;
    size_t *offsets;
    size_t offsets_cap;
    uint32_t max_obj;
} PdfWriter;

void pdfw_init(PdfWriter *w);
void pdfw_init_stream(PdfWriter *w, FILE *stream, size_t max_stream_size);
void pdfw_free(PdfWriter *w);
void pdfw_start_object(PdfWriter *w, uint32_t objnum);
void pdfw_end_object(PdfWriter *w);
void pdfw_put(PdfWriter *w, const void *bytes, size_t len);
void pdfw_puts(PdfWriter *w, const char *text);
void pdfw_printf(PdfWriter *w, const char *fmt, ...);
int pdfw_finish(PdfWriter *w, uint32_t root_obj, uint32_t info_obj);
int pdfw_finish_simple(PdfWriter *w, uint32_t root_obj);

#endif

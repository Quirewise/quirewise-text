#ifndef QUIREWISE_PML_H
#define QUIREWISE_PML_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(QUIREWISE_PML_BUILD_SHARED)
#  if defined(QUIREWISE_PML_EXPORTS)
#    define QUIREWISE_PML_API __declspec(dllexport)
#  else
#    define QUIREWISE_PML_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(QUIREWISE_PML_BUILD_SHARED)
#  define QUIREWISE_PML_API __attribute__((visibility("default")))
#else
#  define QUIREWISE_PML_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QUIREWISE_PML_ABI_VERSION 1u

typedef enum QwPmlStatus {
    QW_PML_OK = 0,
    QW_PML_ERR_INVALID_ARGUMENT = -1,
    QW_PML_ERR_OUT_OF_MEMORY = -2,
    QW_PML_ERR_RESOURCE_LIMIT = -3
} QwPmlStatus;

/* The parser strips the supported basic PML commands and preserves the input
 * byte encoding. text_bytes is owned by the document. */
typedef struct QwPmlDocument {
    uint8_t *text_bytes;
    size_t text_size;
} QwPmlDocument;

QUIREWISE_PML_API uint32_t qw_pml_abi_version(void);

/* Accepts non-empty PML input up to 64 MiB. It handles paragraph, newline,
 * tab, escaped backslash, and basic letter commands. */
QUIREWISE_PML_API QwPmlStatus qw_pml_parse(
    const void *pml_bytes, size_t pml_size, QwPmlDocument *out_document);
QUIREWISE_PML_API void qw_pml_document_free(QwPmlDocument *document);
QUIREWISE_PML_API const char *qw_pml_status_string(int status);

#ifdef __cplusplus
}
#endif

#endif

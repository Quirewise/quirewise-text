#ifndef QUIREWISE_TEXT_ARCHIVE_H
#define QUIREWISE_TEXT_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(QUIREWISE_TEXT_ARCHIVE_BUILD_SHARED)
#  if defined(QUIREWISE_TEXT_ARCHIVE_EXPORTS)
#    define QUIREWISE_TEXT_ARCHIVE_API __declspec(dllexport)
#  else
#    define QUIREWISE_TEXT_ARCHIVE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(QUIREWISE_TEXT_ARCHIVE_BUILD_SHARED)
#  define QUIREWISE_TEXT_ARCHIVE_API __attribute__((visibility("default")))
#else
#  define QUIREWISE_TEXT_ARCHIVE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QUIREWISE_TEXT_ARCHIVE_ABI_VERSION 1u

typedef enum QwTextArchiveKind {
    QW_TEXT_ARCHIVE_TXTZ = 1,
    QW_TEXT_ARCHIVE_PMLZ = 2
} QwTextArchiveKind;

typedef enum QwTextArchiveStatus {
    QW_TEXT_ARCHIVE_OK = 0,
    QW_TEXT_ARCHIVE_ERR_INVALID_ARGUMENT = -1,
    QW_TEXT_ARCHIVE_ERR_OUT_OF_MEMORY = -2,
    QW_TEXT_ARCHIVE_ERR_ARCHIVE = -3,
    QW_TEXT_ARCHIVE_ERR_NOT_FOUND = -4,
    QW_TEXT_ARCHIVE_ERR_RESOURCE_LIMIT = -5
} QwTextArchiveStatus;

/* text_bytes and member_name_bytes are owned by the document. The name bytes
 * retain the ZIP entry-name encoding and are not guaranteed to be UTF-8. */
typedef struct QwTextArchiveDocument {
    uint8_t *text_bytes;
    size_t text_size;
    uint8_t *member_name_bytes;
    size_t member_name_size;
    size_t archive_entry_index;
} QwTextArchiveDocument;

QUIREWISE_TEXT_ARCHIVE_API uint32_t qw_text_archive_abi_version(void);

/* Extracts index.txt/index.pml first, then the first matching text member with
 * text-like contents. Input bytes are borrowed; release output with
 * qw_text_archive_document_free. out_error, when set, points to a static
 * diagnostic string owned by the library. */
QUIREWISE_TEXT_ARCHIVE_API QwTextArchiveStatus qw_text_archive_extract_primary(
    const void *archive_bytes, size_t archive_size, QwTextArchiveKind kind,
    QwTextArchiveDocument *out_document, const char **out_error);
QUIREWISE_TEXT_ARCHIVE_API void qw_text_archive_document_free(
    QwTextArchiveDocument *document);
QUIREWISE_TEXT_ARCHIVE_API const char *qw_text_archive_status_string(int status);

#ifdef __cplusplus
}
#endif

#endif

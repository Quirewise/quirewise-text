/*
 * Markdown rendering through the public text API: headings, emphasis and
 * links become an HTML fragment.
 */

#include <quirewise/markdown.h>

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void)
{
    static const char markdown[] =
        "# Title\n"
        "\n"
        "Some *emphasis* and **strong** text with a [link](https://example.com).\n"
        "\n"
        "- first\n"
        "- second\n";
    QwMarkdownDocument document;

    memset(&document, 0, sizeof(document));
    CHECK(qw_markdown_render_html(markdown, sizeof(markdown) - 1u, &document) ==
          QW_MARKDOWN_OK);
    CHECK(document.html != NULL && document.html_size > 0u);
    CHECK(strstr(document.html, "<h1") != NULL);
    CHECK(strstr(document.html, "Title") != NULL);
    CHECK(strstr(document.html, "<em>") != NULL);
    CHECK(strstr(document.html, "<strong>") != NULL);
    CHECK(strstr(document.html, "https://example.com") != NULL);
    CHECK(strstr(document.html, "<li>") != NULL);
    qw_markdown_document_free(&document);

    /* Empty input is rejected rather than silently accepted. */
    memset(&document, 0, sizeof(document));
    CHECK(qw_markdown_render_html("", 0u, &document) != QW_MARKDOWN_OK);
    return 0;
}

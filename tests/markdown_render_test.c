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

    {
        static const char table[] =
            "| Left | Center | Right |\n"
            "| :--- | :---: | ---: |\n"
            "| a | b | c |\n";
        memset(&document, 0, sizeof(document));
        CHECK(qw_markdown_render_html(table, sizeof(table) - 1u, &document) ==
              QW_MARKDOWN_OK);
        CHECK(document.html != NULL);
        CHECK(strstr(document.html, "<table>") != NULL);
        CHECK(strstr(document.html, "<th align=\"left\">") != NULL);
        CHECK(strstr(document.html, "<th align=\"center\">") != NULL);
        CHECK(strstr(document.html, "<th align=\"right\">") != NULL);
        CHECK(strstr(document.html, "<td align=\"center\">") != NULL);
        qw_markdown_document_free(&document);
    }

    {
        static const char escaped[] =
            "| A | B |\n"
            "| --- | --- |\n"
            "| a \\| b | c |\n";
        memset(&document, 0, sizeof(document));
        CHECK(qw_markdown_render_html(escaped, sizeof(escaped) - 1u,
                                      &document) == QW_MARKDOWN_OK);
        CHECK(document.html != NULL);
        CHECK(strstr(document.html, "a | b") != NULL);
        CHECK(strstr(document.html, "<td>c</td>") != NULL);
        qw_markdown_document_free(&document);
    }

    {
        static const char setext[] =
            "Setext One\n"
            "=====\n"
            "\n"
            "Setext Two\n"
            "---\n";
        memset(&document, 0, sizeof(document));
        CHECK(qw_markdown_render_html(setext, sizeof(setext) - 1u, &document) ==
              QW_MARKDOWN_OK);
        CHECK(document.html != NULL);
        CHECK(strstr(document.html, "<h1>Setext One</h1>") != NULL);
        CHECK(strstr(document.html, "<h2>Setext Two</h2>") != NULL);
        qw_markdown_document_free(&document);
    }

    {
        static const char fenced[] =
            "```c\n"
            "int value = 1;\n"
            "```\n";
        memset(&document, 0, sizeof(document));
        CHECK(qw_markdown_render_html(fenced, sizeof(fenced) - 1u,
                                      &document) == QW_MARKDOWN_OK);
        CHECK(document.html != NULL);
        CHECK(strstr(document.html, "class=\"language-c\"") != NULL);
        CHECK(strstr(document.html, "int value = 1;") != NULL);
        qw_markdown_document_free(&document);
    }

    {
        static const char tasks[] =
            "- [x] done\n"
            "- [ ] todo\n";
        memset(&document, 0, sizeof(document));
        CHECK(qw_markdown_render_html(tasks, sizeof(tasks) - 1u, &document) ==
              QW_MARKDOWN_OK);
        CHECK(document.html != NULL);
        CHECK(strstr(document.html,
                     "<input type=\"checkbox\" disabled checked>") != NULL);
        CHECK(strstr(document.html,
                     "<input type=\"checkbox\" disabled> todo") != NULL);
        qw_markdown_document_free(&document);
    }

    /* Empty input is rejected rather than silently accepted. */
    memset(&document, 0, sizeof(document));
    CHECK(qw_markdown_render_html("", 0u, &document) != QW_MARKDOWN_OK);
    return 0;
}

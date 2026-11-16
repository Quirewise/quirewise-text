/* HTML -> Markdown writer tests. */

#include <quirewise/markdown.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static int contains(const char *text, const char *needle)
{
    return text && strstr(text, needle) != NULL;
}

static void expect(const char *label, const char *html, const char *needle)
{
    QwMarkdownText document;

    memset(&document, 0, sizeof(document));
    if (qw_markdown_from_html(html, strlen(html), &document) != QW_MARKDOWN_OK ||
        !document.markdown) {
        fprintf(stderr, "FAIL: conversion failed for %s\n", label);
        exit(1);
    }
    if (!contains(document.markdown, needle)) {
        fprintf(stderr, "FAIL: %s lost %s\n---\n%s\n---\n", label, needle,
                document.markdown);
        exit(1);
    }
    qw_markdown_text_free(&document);
    if (document.markdown) fail("free did not clear the buffer");
}

int main(void)
{
    QwMarkdownText document;

    expect("heading", "<html><head><title>x</title></head><body>"
                      "<h1>Title</h1><p>Body</p></body></html>",
           "# Title");
    expect("paragraph", "<p>Hello <strong>bold</strong> and "
                        "<em>italic</em>.</p>",
           "Hello **bold** and *italic*.");
    expect("link", "<p><a href=\"https://example.com\">site</a></p>",
           "[site](https://example.com)");
    expect("list", "<ul><li>one</li><li>two</li></ul>",
           "- one");
    expect("ordered list", "<ol><li>first</li></ol>", "1. first");
    expect("blockquote", "<blockquote><p>quoted</p></blockquote>", "> quoted");
    expect("inline code", "<p>Use <code>qw_x</code> here</p>", "`qw_x`");
    expect("fenced code", "<pre>line 1\nline 2</pre>", "```\nline 1");
    expect("table", "<table><tr><th>a</th><th>b</th></tr>"
                    "<tr><td>1</td><td>2</td></tr></table>",
           "| a | b |");
    expect("table alignment",
           "<table><tr><th align=\"center\">A</th>"
           "<th align=\"right\">B</th><th align=\"left\">C</th>"
           "<th>D</th></tr><tr><td>1</td><td>2</td><td>3</td>"
           "<td>4</td></tr></table>",
           "| :---: | ---: | :--- | --- |");
    expect("entity", "<p>a &amp; b &lt;3</p>", "a & b <3");
    expect("image alt", "<p><img src=\"x.png\" alt=\"diagram\"></p>",
           "![diagram](x.png)");
    expect("image data", "<p><img alt=\"shot\" src=\"data:image/png;base64,AA\"></p>",
           "![shot](data:image/png;base64,AA)");
    expect("image with width", "<p><img alt=\"diagram\" width=\"64\" src=\"x.png\"></p>",
           "![diagram](x.png)");
    expect("image no src", "<p><img alt=\"orphan\"></p>", "orphan");
    expect("script skipped", "<p>keep</p><script>drop()</script>",
           "keep");

    /* The script/style content must not survive. */
    memset(&document, 0, sizeof(document));
    if (qw_markdown_from_html(
            "<script>bad()</script><style>.x{}</style><p>good</p>",
            strlen("<script>bad()</script><style>.x{}</style><p>good</p>"),
            &document) != QW_MARKDOWN_OK ||
        !document.markdown)
        fail("conversion failed");
    if (strstr(document.markdown, "bad()") ||
        strstr(document.markdown, ".x{}"))
        fail("script/style content survived");
    if (!contains(document.markdown, "good")) fail("body text was dropped");
    qw_markdown_text_free(&document);

    /* Empty and invalid inputs are refused. */
    memset(&document, 0, sizeof(document));
    if (qw_markdown_from_html(NULL, 0u, &document) !=
        QW_MARKDOWN_ERR_INVALID_ARGUMENT)
        fail("NULL input was accepted");
    if (document.markdown) fail("failed conversion produced output");
    qw_markdown_text_free(NULL);
    puts("Quirewise HTML to Markdown tests passed");
    return 0;
}

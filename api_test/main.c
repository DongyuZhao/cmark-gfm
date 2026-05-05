#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CMARK_NO_SHORT_NAMES
#include "cmark-gfm.h"
#include "node.h"
#include "parser.h"
#include "../extensions/cmark-gfm-core-extensions.h"

#include "harness.h"
#include "cplusplus.h"

#define UTF8_REPL "\xEF\xBF\xBD"

static const cmark_node_type node_types[] = {
    CMARK_NODE_DOCUMENT,  CMARK_NODE_BLOCK_QUOTE, CMARK_NODE_LIST,
    CMARK_NODE_ITEM,      CMARK_NODE_CODE_BLOCK,  CMARK_NODE_HTML_BLOCK,
    CMARK_NODE_PARAGRAPH, CMARK_NODE_HEADING,     CMARK_NODE_THEMATIC_BREAK,
    CMARK_NODE_TEXT,      CMARK_NODE_SOFTBREAK,   CMARK_NODE_LINEBREAK,
    CMARK_NODE_CODE,      CMARK_NODE_HTML_INLINE, CMARK_NODE_EMPH,
    CMARK_NODE_STRONG,    CMARK_NODE_LINK,        CMARK_NODE_IMAGE};
static const int num_node_types = sizeof(node_types) / sizeof(*node_types);

static void test_md_to_html(test_batch_runner *runner, const char *markdown,
                            const char *expected_html, const char *msg);

static void test_content(test_batch_runner *runner, cmark_node_type type,
                         unsigned int *allowed_content);

static void test_char(test_batch_runner *runner, int valid, const char *utf8,
                      const char *msg);

static void test_incomplete_char(test_batch_runner *runner, const char *utf8,
                                 const char *msg);

static void test_continuation_byte(test_batch_runner *runner, const char *utf8);

static void attach_gfm_core_extensions(cmark_parser *parser) {
  const char *exts[] = {"table", "strikethrough", "autolink", "tasklist"};
  for (size_t i = 0; i < sizeof(exts) / sizeof(*exts); ++i) {
    cmark_syntax_extension *ext = cmark_find_syntax_extension(exts[i]);
    if (ext) {
      cmark_parser_attach_syntax_extension(parser, ext);
    }
  }
}

static void version(test_batch_runner *runner) {
  INT_EQ(runner, cmark_version(), CMARK_GFM_VERSION, "cmark_version");
  STR_EQ(runner, cmark_version_string(), CMARK_GFM_VERSION_STRING,
         "cmark_version_string");
}

static void constructor(test_batch_runner *runner) {
  for (int i = 0; i < num_node_types; ++i) {
    cmark_node_type type = node_types[i];
    cmark_node *node = cmark_node_new(type);
    OK(runner, node != NULL, "new type %d", type);
    INT_EQ(runner, cmark_node_get_type(node), type, "get_type %d", type);

    switch (node->type) {
    case CMARK_NODE_HEADING:
      INT_EQ(runner, cmark_node_get_heading_level(node), 1,
             "default heading level is 1");
      node->as.heading.level = 1;
      break;

    case CMARK_NODE_LIST:
      INT_EQ(runner, cmark_node_get_list_type(node), CMARK_BULLET_LIST,
             "default is list type is bullet");
      INT_EQ(runner, cmark_node_get_list_delim(node), CMARK_NO_DELIM,
             "default is list delim is NO_DELIM");
      INT_EQ(runner, cmark_node_get_list_start(node), 0,
             "default is list start is 0");
      INT_EQ(runner, cmark_node_get_list_tight(node), 0,
             "default is list is loose");
      break;

    default:
      break;
    }

    cmark_node_free(node);
  }
}

static void accessors(test_batch_runner *runner) {
  static const char markdown[] = "## Header\n"
                                 "\n"
                                 "* Item 1\n"
                                 "* Item 2\n"
                                 "\n"
                                 "2. Item 1\n"
                                 "\n"
                                 "3. Item 2\n"
                                 "\n"
                                 "``` lang\n"
                                 "fenced\n"
                                 "```\n"
                                 "    code\n"
                                 "\n"
                                 "<div>html</div>\n"
                                 "\n"
                                 "[link](url 'title')\n";

  cmark_node *doc =
      cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);

  // Getters

  cmark_node *heading = cmark_node_first_child(doc);
  INT_EQ(runner, cmark_node_get_heading_level(heading), 2, "get_heading_level");

  cmark_node *bullet_list = cmark_node_next(heading);
  INT_EQ(runner, cmark_node_get_list_type(bullet_list), CMARK_BULLET_LIST,
         "get_list_type bullet");
  INT_EQ(runner, cmark_node_get_list_tight(bullet_list), 1,
         "get_list_tight tight");

  cmark_node *ordered_list = cmark_node_next(bullet_list);
  INT_EQ(runner, cmark_node_get_list_type(ordered_list), CMARK_ORDERED_LIST,
         "get_list_type ordered");
  INT_EQ(runner, cmark_node_get_list_delim(ordered_list), CMARK_PERIOD_DELIM,
         "get_list_delim ordered");
  INT_EQ(runner, cmark_node_get_list_start(ordered_list), 2, "get_list_start");
  INT_EQ(runner, cmark_node_get_list_tight(ordered_list), 0,
         "get_list_tight loose");

  cmark_node *fenced = cmark_node_next(ordered_list);
  STR_EQ(runner, cmark_node_get_literal(fenced), "fenced\n",
         "get_literal fenced code");
  STR_EQ(runner, cmark_node_get_fence_info(fenced), "lang", "get_fence_info");

  cmark_node *code = cmark_node_next(fenced);
  STR_EQ(runner, cmark_node_get_literal(code), "code\n",
         "get_literal indented code");

  cmark_node *html = cmark_node_next(code);
  STR_EQ(runner, cmark_node_get_literal(html), "<div>html</div>\n",
         "get_literal html");

  cmark_node *paragraph = cmark_node_next(html);
  INT_EQ(runner, cmark_node_get_start_line(paragraph), 17, "get_start_line");
  INT_EQ(runner, cmark_node_get_start_column(paragraph), 1, "get_start_column");
  INT_EQ(runner, cmark_node_get_end_line(paragraph), 17, "get_end_line");

  cmark_node *link = cmark_node_first_child(paragraph);
  STR_EQ(runner, cmark_node_get_url(link), "url", "get_url");
  STR_EQ(runner, cmark_node_get_title(link), "title", "get_title");

  cmark_node *string = cmark_node_first_child(link);
  STR_EQ(runner, cmark_node_get_literal(string), "link", "get_literal string");

  // Setters

  OK(runner, cmark_node_set_heading_level(heading, 3), "set_heading_level");

  OK(runner, cmark_node_set_list_type(bullet_list, CMARK_ORDERED_LIST),
     "set_list_type ordered");
  OK(runner, cmark_node_set_list_delim(bullet_list, CMARK_PAREN_DELIM),
     "set_list_delim paren");
  OK(runner, cmark_node_set_list_start(bullet_list, 3), "set_list_start");
  OK(runner, cmark_node_set_list_tight(bullet_list, 0), "set_list_tight loose");

  OK(runner, cmark_node_set_list_type(ordered_list, CMARK_BULLET_LIST),
     "set_list_type bullet");
  OK(runner, cmark_node_set_list_tight(ordered_list, 1),
     "set_list_tight tight");

  OK(runner, cmark_node_set_literal(code, "CODE\n"),
     "set_literal indented code");

  OK(runner, cmark_node_set_literal(fenced, "FENCED\n"),
     "set_literal fenced code");
  OK(runner, cmark_node_set_fence_info(fenced, "LANG"), "set_fence_info");

  OK(runner, cmark_node_set_literal(html, "<div>HTML</div>\n"),
     "set_literal html");

  OK(runner, cmark_node_set_url(link, "URL"), "set_url");
  OK(runner, cmark_node_set_title(link, "TITLE"), "set_title");

  OK(runner, cmark_node_set_literal(string, "prefix-LINK"),
     "set_literal string");

  // Set literal to suffix of itself (issue #139).
  const char *literal = cmark_node_get_literal(string);
  OK(runner, cmark_node_set_literal(string, literal + sizeof("prefix")),
     "set_literal suffix");

  char *rendered_html = cmark_render_html(doc, CMARK_OPT_DEFAULT | CMARK_OPT_UNSAFE, NULL);
  static const char expected_html[] =
      "<h3>Header</h3>\n"
      "<ol start=\"3\">\n"
      "<li>\n"
      "<p>Item 1</p>\n"
      "</li>\n"
      "<li>\n"
      "<p>Item 2</p>\n"
      "</li>\n"
      "</ol>\n"
      "<ul>\n"
      "<li>Item 1</li>\n"
      "<li>Item 2</li>\n"
      "</ul>\n"
      "<pre><code class=\"language-LANG\">FENCED\n"
      "</code></pre>\n"
      "<pre><code>CODE\n"
      "</code></pre>\n"
      "<div>HTML</div>\n"
      "<p><a href=\"URL\" title=\"TITLE\">LINK</a></p>\n";
  STR_EQ(runner, rendered_html, expected_html, "setters work");
  free(rendered_html);

  // Getter errors

  INT_EQ(runner, cmark_node_get_heading_level(bullet_list), 0,
         "get_heading_level error");
  INT_EQ(runner, cmark_node_get_list_type(heading), CMARK_NO_LIST,
         "get_list_type error");
  INT_EQ(runner, cmark_node_get_list_start(code), 0, "get_list_start error");
  INT_EQ(runner, cmark_node_get_list_tight(fenced), 0, "get_list_tight error");
  OK(runner, cmark_node_get_literal(ordered_list) == NULL, "get_literal error");
  OK(runner, cmark_node_get_fence_info(paragraph) == NULL,
     "get_fence_info error");
  OK(runner, cmark_node_get_url(html) == NULL, "get_url error");
  OK(runner, cmark_node_get_title(heading) == NULL, "get_title error");

  // Setter errors

  OK(runner, !cmark_node_set_heading_level(bullet_list, 3),
     "set_heading_level error");
  OK(runner, !cmark_node_set_list_type(heading, CMARK_ORDERED_LIST),
     "set_list_type error");
  OK(runner, !cmark_node_set_list_start(code, 3), "set_list_start error");
  OK(runner, !cmark_node_set_list_tight(fenced, 0), "set_list_tight error");
  OK(runner, !cmark_node_set_literal(ordered_list, "content\n"),
     "set_literal error");
  OK(runner, !cmark_node_set_fence_info(paragraph, "lang"),
     "set_fence_info error");
  OK(runner, !cmark_node_set_url(html, "url"), "set_url error");
  OK(runner, !cmark_node_set_title(heading, "title"), "set_title error");

  OK(runner, !cmark_node_set_heading_level(heading, 0),
     "set_heading_level too small");
  OK(runner, !cmark_node_set_heading_level(heading, 7),
     "set_heading_level too large");
  OK(runner, !cmark_node_set_list_type(bullet_list, CMARK_NO_LIST),
     "set_list_type invalid");
  OK(runner, !cmark_node_set_list_start(bullet_list, -1),
     "set_list_start negative");

  cmark_node_free(doc);
}

static void node_check(test_batch_runner *runner) {
  // Construct an incomplete tree.
  cmark_node *doc = cmark_node_new(CMARK_NODE_DOCUMENT);
  cmark_node *p1 = cmark_node_new(CMARK_NODE_PARAGRAPH);
  cmark_node *p2 = cmark_node_new(CMARK_NODE_PARAGRAPH);
  doc->first_child = p1;
  p1->next = p2;

  INT_EQ(runner, cmark_node_check(doc, NULL), 4, "node_check works");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "node_check fixes tree");

  cmark_node_free(doc);
}

static void iterator(test_batch_runner *runner) {
  cmark_node *doc = cmark_parse_document("> a *b*\n\nc", 10, CMARK_OPT_DEFAULT);
  int parnodes = 0;
  cmark_event_type ev_type;
  cmark_iter *iter = cmark_iter_new(doc);
  cmark_node *cur;

  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cur = cmark_iter_get_node(iter);
    if (cur->type == CMARK_NODE_PARAGRAPH && ev_type == CMARK_EVENT_ENTER) {
      parnodes += 1;
    }
  }
  INT_EQ(runner, parnodes, 2, "iterate correctly counts paragraphs");

  cmark_iter_free(iter);
  cmark_node_free(doc);
}

static void iterator_delete(test_batch_runner *runner) {
  static const char md[] = "a *b* c\n"
                           "\n"
                           "* item1\n"
                           "* item2\n"
                           "\n"
                           "a `b` c\n"
                           "\n"
                           "* item1\n"
                           "* item2\n";
  cmark_node *doc = cmark_parse_document(md, sizeof(md) - 1, CMARK_OPT_DEFAULT);
  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev_type;

  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    // Delete list, emph, and code nodes.
    if ((ev_type == CMARK_EVENT_EXIT && node->type == CMARK_NODE_LIST) ||
        (ev_type == CMARK_EVENT_EXIT && node->type == CMARK_NODE_EMPH) ||
        (ev_type == CMARK_EVENT_ENTER && node->type == CMARK_NODE_CODE)) {
      cmark_node_free(node);
    }
  }

  char *html = cmark_render_html(doc, CMARK_OPT_DEFAULT, NULL);
  static const char expected[] = "<p>a  c</p>\n"
                                 "<p>a  c</p>\n";
  STR_EQ(runner, html, expected, "iterate and delete nodes");

  free(html);
  cmark_iter_free(iter);
  cmark_node_free(doc);
}

static void create_tree(test_batch_runner *runner) {
  char *html;
  cmark_node *doc = cmark_node_new(CMARK_NODE_DOCUMENT);

  cmark_node *p = cmark_node_new(CMARK_NODE_PARAGRAPH);
  OK(runner, !cmark_node_insert_before(doc, p), "insert before root fails");
  OK(runner, !cmark_node_insert_after(doc, p), "insert after root fails");
  OK(runner, cmark_node_append_child(doc, p), "append1");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "append1 consistent");
  OK(runner, cmark_node_parent(p) == doc, "node_parent");

  cmark_node *emph = cmark_node_new(CMARK_NODE_EMPH);
  OK(runner, cmark_node_prepend_child(p, emph), "prepend1");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "prepend1 consistent");

  cmark_node *str1 = cmark_node_new(CMARK_NODE_TEXT);
  cmark_node_set_literal(str1, "Hello, ");
  OK(runner, cmark_node_prepend_child(p, str1), "prepend2");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "prepend2 consistent");

  cmark_node *str3 = cmark_node_new(CMARK_NODE_TEXT);
  cmark_node_set_literal(str3, "!");
  OK(runner, cmark_node_append_child(p, str3), "append2");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "append2 consistent");

  cmark_node *str2 = cmark_node_new(CMARK_NODE_TEXT);
  cmark_node_set_literal(str2, "world");
  OK(runner, cmark_node_append_child(emph, str2), "append3");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "append3 consistent");

  html = cmark_render_html(doc, CMARK_OPT_DEFAULT, NULL);
  STR_EQ(runner, html, "<p>Hello, <em>world</em>!</p>\n", "render_html");
  free(html);

  OK(runner, cmark_node_insert_before(str1, str3), "ins before1");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "ins before1 consistent");
  // 31e
  OK(runner, cmark_node_first_child(p) == str3, "ins before1 works");

  OK(runner, cmark_node_insert_before(str1, emph), "ins before2");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "ins before2 consistent");
  // 3e1
  OK(runner, cmark_node_last_child(p) == str1, "ins before2 works");

  OK(runner, cmark_node_insert_after(str1, str3), "ins after1");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "ins after1 consistent");
  // e13
  OK(runner, cmark_node_next(str1) == str3, "ins after1 works");

  OK(runner, cmark_node_insert_after(str1, emph), "ins after2");
  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "ins after2 consistent");
  // 1e3
  OK(runner, cmark_node_previous(emph) == str1, "ins after2 works");

  cmark_node *str4 = cmark_node_new(CMARK_NODE_TEXT);
  cmark_node_set_literal(str4, "brzz");
  OK(runner, cmark_node_replace(str1, str4), "replace");
  // The replaced node is not freed
  cmark_node_free(str1);

  INT_EQ(runner, cmark_node_check(doc, NULL), 0, "replace consistent");
  OK(runner, cmark_node_previous(emph) == str4, "replace works");
  INT_EQ(runner, cmark_node_replace(p, str4), 0, "replace str for p fails");

  cmark_node_unlink(emph);

  html = cmark_render_html(doc, CMARK_OPT_DEFAULT, NULL);
  STR_EQ(runner, html, "<p>brzz!</p>\n", "render_html after shuffling");
  free(html);

  cmark_node_free(doc);

  // TODO: Test that the contents of an unlinked inline are valid
  // after the parent block was destroyed. This doesn't work so far.
  cmark_node_free(emph);
}

static void custom_nodes(test_batch_runner *runner) {
  char *html;
  char *man;
  cmark_node *doc = cmark_node_new(CMARK_NODE_DOCUMENT);
  cmark_node *p = cmark_node_new(CMARK_NODE_PARAGRAPH);
  cmark_node_append_child(doc, p);
  cmark_node *ci = cmark_node_new(CMARK_NODE_CUSTOM_INLINE);
  cmark_node *str1 = cmark_node_new(CMARK_NODE_TEXT);
  cmark_node_set_literal(str1, "Hello");
  OK(runner, cmark_node_append_child(ci, str1), "append1");
  OK(runner, cmark_node_set_on_enter(ci, "<ON ENTER|"), "set_on_enter");
  OK(runner, cmark_node_set_on_exit(ci, "|ON EXIT>"), "set_on_exit");
  STR_EQ(runner, cmark_node_get_on_enter(ci), "<ON ENTER|", "get_on_enter");
  STR_EQ(runner, cmark_node_get_on_exit(ci), "|ON EXIT>", "get_on_exit");
  cmark_node_append_child(p, ci);
  cmark_node *cb = cmark_node_new(CMARK_NODE_CUSTOM_BLOCK);
  cmark_node_set_on_enter(cb, "<on enter|");
  // leave on_exit unset
  STR_EQ(runner, cmark_node_get_on_exit(cb), "", "get_on_exit (empty)");
  cmark_node_append_child(doc, cb);

  html = cmark_render_html(doc, CMARK_OPT_DEFAULT, NULL);
  STR_EQ(runner, html, "<p><ON ENTER|Hello|ON EXIT></p>\n<on enter|\n",
         "render_html");
  free(html);

  man = cmark_render_man(doc, CMARK_OPT_DEFAULT, 0);
  STR_EQ(runner, man, ".PP\n<ON ENTER|Hello|ON EXIT>\n<on enter|\n",
         "render_man");
  free(man);

  cmark_node_free(doc);
}

void hierarchy(test_batch_runner *runner) {
  cmark_node *bquote1 = cmark_node_new(CMARK_NODE_BLOCK_QUOTE);
  cmark_node *bquote2 = cmark_node_new(CMARK_NODE_BLOCK_QUOTE);
  cmark_node *bquote3 = cmark_node_new(CMARK_NODE_BLOCK_QUOTE);

  OK(runner, cmark_node_append_child(bquote1, bquote2), "append bquote2");
  OK(runner, cmark_node_append_child(bquote2, bquote3), "append bquote3");
  OK(runner, !cmark_node_append_child(bquote3, bquote3),
     "adding a node as child of itself fails");
  OK(runner, !cmark_node_append_child(bquote3, bquote1),
     "adding a parent as child fails");

  cmark_node_free(bquote1);

  unsigned int list_item_flag[] = {CMARK_NODE_ITEM, 0};
  unsigned int top_level_blocks[] = {
    CMARK_NODE_BLOCK_QUOTE, CMARK_NODE_LIST,
    CMARK_NODE_CODE_BLOCK, CMARK_NODE_HTML_BLOCK,
    CMARK_NODE_PARAGRAPH, CMARK_NODE_HEADING,
    CMARK_NODE_THEMATIC_BREAK, 0};
  unsigned int all_inlines[] = {
    CMARK_NODE_TEXT, CMARK_NODE_SOFTBREAK,
    CMARK_NODE_LINEBREAK, CMARK_NODE_CODE,
    CMARK_NODE_HTML_INLINE, CMARK_NODE_EMPH,
    CMARK_NODE_STRONG, CMARK_NODE_LINK,
    CMARK_NODE_IMAGE, 0};

  test_content(runner, CMARK_NODE_DOCUMENT, top_level_blocks);
  test_content(runner, CMARK_NODE_BLOCK_QUOTE, top_level_blocks);
  test_content(runner, CMARK_NODE_LIST, list_item_flag);
  test_content(runner, CMARK_NODE_ITEM, top_level_blocks);
  test_content(runner, CMARK_NODE_CODE_BLOCK, 0);
  test_content(runner, CMARK_NODE_HTML_BLOCK, 0);
  test_content(runner, CMARK_NODE_PARAGRAPH, all_inlines);
  test_content(runner, CMARK_NODE_HEADING, all_inlines);
  test_content(runner, CMARK_NODE_THEMATIC_BREAK, 0);
  test_content(runner, CMARK_NODE_TEXT, 0);
  test_content(runner, CMARK_NODE_SOFTBREAK, 0);
  test_content(runner, CMARK_NODE_LINEBREAK, 0);
  test_content(runner, CMARK_NODE_CODE, 0);
  test_content(runner, CMARK_NODE_HTML_INLINE, 0);
  test_content(runner, CMARK_NODE_EMPH, all_inlines);
  test_content(runner, CMARK_NODE_STRONG, all_inlines);
  test_content(runner, CMARK_NODE_LINK, all_inlines);
  test_content(runner, CMARK_NODE_IMAGE, all_inlines);
}

static void test_content(test_batch_runner *runner, cmark_node_type type,
                         unsigned int *allowed_content) {
  cmark_node *node = cmark_node_new(type);

  for (int i = 0; i < num_node_types; ++i) {
    cmark_node_type child_type = node_types[i];
    cmark_node *child = cmark_node_new(child_type);

    int got = cmark_node_append_child(node, child);
    int expected = 0;
    if (allowed_content)
        for (unsigned int *p = allowed_content; *p; ++p)
            expected |= *p == (unsigned int)child_type;

    INT_EQ(runner, got, expected, "add %d as child of %d", child_type, type);

    cmark_node_free(child);
  }

  cmark_node_free(node);
}

static void parser(test_batch_runner *runner) {
  test_md_to_html(runner, "No newline", "<p>No newline</p>\n",
                  "document without trailing newline");
}

static void render_html(test_batch_runner *runner) {
  char *html;

  static const char markdown[] = "foo *bar*\n"
                                 "\n"
                                 "paragraph 2\n";
  cmark_node *doc =
      cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);

  cmark_node *paragraph = cmark_node_first_child(doc);
  html = cmark_render_html(paragraph, CMARK_OPT_DEFAULT, NULL);
  STR_EQ(runner, html, "<p>foo <em>bar</em></p>\n", "render single paragraph");
  free(html);

  cmark_node *string = cmark_node_first_child(paragraph);
  html = cmark_render_html(string, CMARK_OPT_DEFAULT, NULL);
  STR_EQ(runner, html, "foo ", "render single inline");
  free(html);

  cmark_node *emph = cmark_node_next(string);
  html = cmark_render_html(emph, CMARK_OPT_DEFAULT, NULL);
  STR_EQ(runner, html, "<em>bar</em>", "render inline with children");
  free(html);

  cmark_node_free(doc);
}

static void render_xml(test_batch_runner *runner) {
  char *xml;

  static const char markdown[] = "foo *bar*\n"
                                 "\n"
                                 "paragraph 2\n"
                                 "\n"
                                 "```\ncode\n```\n";
  cmark_node *doc =
      cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);

  xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT);
  STR_EQ(runner, xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                      "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
                      "  <paragraph>\n"
                      "    <text xml:space=\"preserve\">foo </text>\n"
                      "    <emph>\n"
                      "      <text xml:space=\"preserve\">bar</text>\n"
                      "    </emph>\n"
                      "  </paragraph>\n"
                      "  <paragraph>\n"
                      "    <text xml:space=\"preserve\">paragraph 2</text>\n"
                      "  </paragraph>\n"
                      "  <code_block xml:space=\"preserve\">code\n"
                      "</code_block>\n"
                      "</document>\n",
         "render document");
  free(xml);
  cmark_node *paragraph = cmark_node_first_child(doc);
  xml = cmark_render_xml(paragraph, CMARK_OPT_DEFAULT | CMARK_OPT_SOURCEPOS);
  STR_EQ(runner, xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                      "<paragraph sourcepos=\"1:1-1:9\">\n"
                      "  <text sourcepos=\"1:1-1:4\" xml:space=\"preserve\">foo </text>\n"
                      "  <emph sourcepos=\"1:5-1:9\">\n"
                      "    <text sourcepos=\"1:6-1:8\" xml:space=\"preserve\">bar</text>\n"
                      "  </emph>\n"
                      "</paragraph>\n",
         "render first paragraph with source pos");
  free(xml);
  cmark_node_free(doc);
}

static void render_man(test_batch_runner *runner) {
  char *man;

  static const char markdown[] = "foo *bar*\n"
                                 "\n"
                                 "- Lorem ipsum dolor sit amet,\n"
                                 "  consectetur adipiscing elit,\n"
                                 "- sed do eiusmod tempor incididunt\n"
                                 "  ut labore et dolore magna aliqua.\n";
  cmark_node *doc =
      cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);

  man = cmark_render_man(doc, CMARK_OPT_DEFAULT, 20);
  STR_EQ(runner, man, ".PP\n"
                      "foo \\f[I]bar\\f[]\n"
                      ".IP \\[bu] 2\n"
                      "Lorem ipsum dolor\n"
                      "sit amet,\n"
                      "consectetur\n"
                      "adipiscing elit,\n"
                      ".IP \\[bu] 2\n"
                      "sed do eiusmod\n"
                      "tempor incididunt ut\n"
                      "labore et dolore\n"
                      "magna aliqua.\n",
         "render document with wrapping");
  free(man);
  man = cmark_render_man(doc, CMARK_OPT_DEFAULT, 0);
  STR_EQ(runner, man, ".PP\n"
                      "foo \\f[I]bar\\f[]\n"
                      ".IP \\[bu] 2\n"
                      "Lorem ipsum dolor sit amet,\n"
                      "consectetur adipiscing elit,\n"
                      ".IP \\[bu] 2\n"
                      "sed do eiusmod tempor incididunt\n"
                      "ut labore et dolore magna aliqua.\n",
         "render document without wrapping");
  free(man);
  cmark_node_free(doc);
}

static void render_latex(test_batch_runner *runner) {
  char *latex;

  static const char markdown[] = "foo *bar* $%\n"
                                 "\n"
                                 "- Lorem ipsum dolor sit amet,\n"
                                 "  consectetur adipiscing elit,\n"
                                 "- sed do eiusmod tempor incididunt\n"
                                 "  ut labore et dolore magna aliqua.\n";
  cmark_node *doc =
      cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);

  latex = cmark_render_latex(doc, CMARK_OPT_DEFAULT, 20);
  STR_EQ(runner, latex, "foo \\emph{bar} \\$\\%\n"
                        "\n"
                        "\\begin{itemize}\n"
                        "\\item Lorem ipsum\n"
                        "dolor sit amet,\n"
                        "consectetur\n"
                        "adipiscing elit,\n"
                        "\n"
                        "\\item sed do eiusmod\n"
                        "tempor incididunt ut\n"
                        "labore et dolore\n"
                        "magna aliqua.\n"
                        "\n"
                        "\\end{itemize}\n",
         "render document with wrapping");
  free(latex);
  latex = cmark_render_latex(doc, CMARK_OPT_DEFAULT, 0);
  STR_EQ(runner, latex, "foo \\emph{bar} \\$\\%\n"
                        "\n"
                        "\\begin{itemize}\n"
                        "\\item Lorem ipsum dolor sit amet,\n"
                        "consectetur adipiscing elit,\n"
                        "\n"
                        "\\item sed do eiusmod tempor incididunt\n"
                        "ut labore et dolore magna aliqua.\n"
                        "\n"
                        "\\end{itemize}\n",
         "render document without wrapping");
  free(latex);
  cmark_node_free(doc);
}

static void render_commonmark(test_batch_runner *runner) {
  char *commonmark;

  static const char markdown[] = "> \\- foo *bar* \\*bar\\*\n"
                                 "\n"
                                 "- Lorem ipsum dolor sit amet,\n"
                                 "  consectetur adipiscing elit,\n"
                                 "- sed do eiusmod tempor incididunt\n"
                                 "  ut labore et dolore magna aliqua.\n";
  cmark_node *doc =
      cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);

  commonmark = cmark_render_commonmark(doc, CMARK_OPT_DEFAULT, 26);
  STR_EQ(runner, commonmark, "> \\- foo *bar* \\*bar\\*\n"
                             "\n"
                             "  - Lorem ipsum dolor sit\n"
                             "    amet, consectetur\n"
                             "    adipiscing elit,\n"
                             "  - sed do eiusmod tempor\n"
                             "    incididunt ut labore\n"
                             "    et dolore magna\n"
                             "    aliqua.\n",
         "render document with wrapping");
  free(commonmark);
  commonmark = cmark_render_commonmark(doc, CMARK_OPT_DEFAULT, 0);
  STR_EQ(runner, commonmark, "> \\- foo *bar* \\*bar\\*\n"
                             "\n"
                             "  - Lorem ipsum dolor sit amet,\n"
                             "    consectetur adipiscing elit,\n"
                             "  - sed do eiusmod tempor incididunt\n"
                             "    ut labore et dolore magna aliqua.\n",
         "render document without wrapping");
  free(commonmark);

  cmark_node *text = cmark_node_new(CMARK_NODE_TEXT);
  cmark_node_set_literal(text, "Hi");
  commonmark = cmark_render_commonmark(text, CMARK_OPT_DEFAULT, 0);
  STR_EQ(runner, commonmark, "Hi\n", "render single inline node");
  free(commonmark);

  cmark_node_free(text);
  cmark_node_free(doc);
}

static void render_plaintext(test_batch_runner *runner) {
  char *plaintext;

  static const char markdown[] = "> \\- foo *bar* \\*bar\\*\n"
                                 "\n"
                                 "- Lorem ipsum dolor sit amet,\n"
                                 "  consectetur adipiscing elit,\n"
                                 "- sed do eiusmod tempor incididunt\n"
                                 "  ut labore et dolore magna aliqua.\n";
  cmark_node *doc =
      cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);

  plaintext = cmark_render_plaintext(doc, CMARK_OPT_DEFAULT, 26);
  STR_EQ(runner, plaintext, "- foo bar *bar*\n"
                             "\n"
                             "  - Lorem ipsum dolor sit\n"
                             "    amet, consectetur\n"
                             "    adipiscing elit,\n"
                             "  - sed do eiusmod tempor\n"
                             "    incididunt ut labore\n"
                             "    et dolore magna\n"
                             "    aliqua.\n",
         "render document with wrapping");
  free(plaintext);
  plaintext = cmark_render_plaintext(doc, CMARK_OPT_DEFAULT, 0);
  STR_EQ(runner, plaintext, "- foo bar *bar*\n"
                             "\n"
                             "  - Lorem ipsum dolor sit amet,\n"
                             "    consectetur adipiscing elit,\n"
                             "  - sed do eiusmod tempor incididunt\n"
                             "    ut labore et dolore magna aliqua.\n",
         "render document without wrapping");
  free(plaintext);

  cmark_node *text = cmark_node_new(CMARK_NODE_TEXT);
  cmark_node_set_literal(text, "Hi");
  plaintext = cmark_render_plaintext(text, CMARK_OPT_DEFAULT, 0);
  STR_EQ(runner, plaintext, "Hi\n", "render single inline node");
  free(plaintext);

  cmark_node_free(text);
  cmark_node_free(doc);
}

static void utf8(test_batch_runner *runner) {
  // Ranges
  test_char(runner, 1, "\x01", "valid utf8 01");
  test_char(runner, 1, "\x7F", "valid utf8 7F");
  test_char(runner, 0, "\x80", "invalid utf8 80");
  test_char(runner, 0, "\xBF", "invalid utf8 BF");
  test_char(runner, 0, "\xC0\x80", "invalid utf8 C080");
  test_char(runner, 0, "\xC1\xBF", "invalid utf8 C1BF");
  test_char(runner, 1, "\xC2\x80", "valid utf8 C280");
  test_char(runner, 1, "\xDF\xBF", "valid utf8 DFBF");
  test_char(runner, 0, "\xE0\x80\x80", "invalid utf8 E08080");
  test_char(runner, 0, "\xE0\x9F\xBF", "invalid utf8 E09FBF");
  test_char(runner, 1, "\xE0\xA0\x80", "valid utf8 E0A080");
  test_char(runner, 1, "\xED\x9F\xBF", "valid utf8 ED9FBF");
  test_char(runner, 0, "\xED\xA0\x80", "invalid utf8 EDA080");
  test_char(runner, 0, "\xED\xBF\xBF", "invalid utf8 EDBFBF");
  test_char(runner, 0, "\xF0\x80\x80\x80", "invalid utf8 F0808080");
  test_char(runner, 0, "\xF0\x8F\xBF\xBF", "invalid utf8 F08FBFBF");
  test_char(runner, 1, "\xF0\x90\x80\x80", "valid utf8 F0908080");
  test_char(runner, 1, "\xF4\x8F\xBF\xBF", "valid utf8 F48FBFBF");
  test_char(runner, 0, "\xF4\x90\x80\x80", "invalid utf8 F4908080");
  test_char(runner, 0, "\xF7\xBF\xBF\xBF", "invalid utf8 F7BFBFBF");
  test_char(runner, 0, "\xF8", "invalid utf8 F8");
  test_char(runner, 0, "\xFF", "invalid utf8 FF");

  // Incomplete byte sequences at end of input
  test_incomplete_char(runner, "\xE0\xA0", "invalid utf8 E0A0");
  test_incomplete_char(runner, "\xF0\x90\x80", "invalid utf8 F09080");

  // Invalid continuation bytes
  test_continuation_byte(runner, "\xC2\x80");
  test_continuation_byte(runner, "\xE0\xA0\x80");
  test_continuation_byte(runner, "\xF0\x90\x80\x80");

  // Test string containing null character
  static const char string_with_null[] = "((((\0))))";
  char *html = cmark_markdown_to_html(
      string_with_null, sizeof(string_with_null) - 1, CMARK_OPT_DEFAULT);
  STR_EQ(runner, html, "<p>((((" UTF8_REPL "))))</p>\n", "utf8 with U+0000");
  free(html);

  // Test NUL followed by newline
  static const char string_with_nul_lf[] = "```\n\0\n```\n";
  html = cmark_markdown_to_html(
      string_with_nul_lf, sizeof(string_with_nul_lf) - 1, CMARK_OPT_DEFAULT);
  STR_EQ(runner, html, "<pre><code>\xef\xbf\xbd\n</code></pre>\n",
         "utf8 with \\0\\n");
  free(html);

  // Test byte-order marker
  static const char string_with_bom[] = "\xef\xbb\xbf# Hello\n";
  html = cmark_markdown_to_html(
      string_with_bom, sizeof(string_with_bom) - 1, CMARK_OPT_DEFAULT);
  STR_EQ(runner, html, "<h1>Hello</h1>\n", "utf8 with BOM");
  free(html);
}

static void test_char(test_batch_runner *runner, int valid, const char *utf8,
                      const char *msg) {
  char buf[20];
  sprintf(buf, "((((%s))))", utf8);

  if (valid) {
    char expected[30];
    sprintf(expected, "<p>((((%s))))</p>\n", utf8);
    test_md_to_html(runner, buf, expected, msg);
  } else {
    test_md_to_html(runner, buf, "<p>((((" UTF8_REPL "))))</p>\n", msg);
  }
}

static void test_incomplete_char(test_batch_runner *runner, const char *utf8,
                                 const char *msg) {
  char buf[20];
  sprintf(buf, "----%s", utf8);
  test_md_to_html(runner, buf, "<p>----" UTF8_REPL "</p>\n", msg);
}

static void test_continuation_byte(test_batch_runner *runner,
                                   const char *utf8) {
  size_t len = strlen(utf8);

  for (size_t pos = 1; pos < len; ++pos) {
    char buf[20];
    sprintf(buf, "((((%s))))", utf8);
    buf[4 + pos] = '\x20';

    char expected[50];
    strcpy(expected, "<p>((((" UTF8_REPL "\x20");
    for (size_t i = pos + 1; i < len; ++i) {
      strcat(expected, UTF8_REPL);
    }
    strcat(expected, "))))</p>\n");

    char *html =
        cmark_markdown_to_html(buf, strlen(buf), CMARK_OPT_VALIDATE_UTF8);
    STR_EQ(runner, html, expected, "invalid utf8 continuation byte %zu/%zu", pos,
           len);
    free(html);
  }
}

static void line_endings(test_batch_runner *runner) {
  // Test list with different line endings
  static const char list_with_endings[] = "- a\n- b\r\n- c\r- d";
  char *html = cmark_markdown_to_html(
      list_with_endings, sizeof(list_with_endings) - 1, CMARK_OPT_DEFAULT);
  STR_EQ(runner, html,
         "<ul>\n<li>a</li>\n<li>b</li>\n<li>c</li>\n<li>d</li>\n</ul>\n",
         "list with different line endings");
  free(html);

  static const char crlf_lines[] = "line\r\nline\r\n";
  html = cmark_markdown_to_html(crlf_lines, sizeof(crlf_lines) - 1,
                                CMARK_OPT_DEFAULT | CMARK_OPT_HARDBREAKS);
  STR_EQ(runner, html, "<p>line<br />\nline</p>\n",
         "crlf endings with CMARK_OPT_HARDBREAKS");
  free(html);
  html = cmark_markdown_to_html(crlf_lines, sizeof(crlf_lines) - 1,
                                CMARK_OPT_DEFAULT | CMARK_OPT_NOBREAKS);
  STR_EQ(runner, html, "<p>line line</p>\n",
         "crlf endings with CMARK_OPT_NOBREAKS");
  free(html);

  static const char no_line_ending[] = "```\nline\n```";
  html = cmark_markdown_to_html(no_line_ending, sizeof(no_line_ending) - 1,
                                CMARK_OPT_DEFAULT);
  STR_EQ(runner, html, "<pre><code>line\n</code></pre>\n",
         "fenced code block with no final newline");
  free(html);
}

static void numeric_entities(test_batch_runner *runner) {
  test_md_to_html(runner, "&#0;", "<p>" UTF8_REPL "</p>\n",
                  "Invalid numeric entity 0");
  test_md_to_html(runner, "&#55295;", "<p>\xED\x9F\xBF</p>\n",
                  "Valid numeric entity 0xD7FF");
  test_md_to_html(runner, "&#xD800;", "<p>" UTF8_REPL "</p>\n",
                  "Invalid numeric entity 0xD800");
  test_md_to_html(runner, "&#xDFFF;", "<p>" UTF8_REPL "</p>\n",
                  "Invalid numeric entity 0xDFFF");
  test_md_to_html(runner, "&#57344;", "<p>\xEE\x80\x80</p>\n",
                  "Valid numeric entity 0xE000");
  test_md_to_html(runner, "&#x10FFFF;", "<p>\xF4\x8F\xBF\xBF</p>\n",
                  "Valid numeric entity 0x10FFFF");
  test_md_to_html(runner, "&#x110000;", "<p>" UTF8_REPL "</p>\n",
                  "Invalid numeric entity 0x110000");
  test_md_to_html(runner, "&#x80000000;", "<p>" UTF8_REPL "</p>\n",
                  "Invalid numeric entity 0x80000000");
  test_md_to_html(runner, "&#xFFFFFFFF;", "<p>" UTF8_REPL "</p>\n",
                  "Invalid numeric entity 0xFFFFFFFF");
  test_md_to_html(runner, "&#99999999;", "<p>" UTF8_REPL "</p>\n",
                  "Invalid numeric entity 99999999");

  test_md_to_html(runner, "&#;", "<p>&amp;#;</p>\n",
                  "Min decimal entity length");
  test_md_to_html(runner, "&#x;", "<p>&amp;#x;</p>\n",
                  "Min hexadecimal entity length");
  test_md_to_html(runner, "&#999999999;", "<p>&amp;#999999999;</p>\n",
                  "Max decimal entity length");
  test_md_to_html(runner, "&#x000000041;", "<p>&amp;#x000000041;</p>\n",
                  "Max hexadecimal entity length");
}

static void test_safe(test_batch_runner *runner) {
  // Test safe mode
  static const char raw_html[] = "<div>\nhi\n</div>\n\n<a>hi</"
                                 "a>\n[link](JAVAscript:alert('hi'))\n![image]("
                                 "file:my.js)\n";
  char *html = cmark_markdown_to_html(raw_html, sizeof(raw_html) - 1,
                                      CMARK_OPT_DEFAULT);
  STR_EQ(runner, html, "<!-- raw HTML omitted -->\n<p><!-- raw HTML omitted "
                       "-->hi<!-- raw HTML omitted -->\n<a "
                       "href=\"\">link</a>\n<img src=\"\" alt=\"image\" "
                       "/></p>\n",
         "input with raw HTML and dangerous links");
  free(html);
}

static void test_md_to_html(test_batch_runner *runner, const char *markdown,
                            const char *expected_html, const char *msg) {
  char *html = cmark_markdown_to_html(markdown, strlen(markdown),
                                      CMARK_OPT_VALIDATE_UTF8);
  STR_EQ(runner, html, expected_html, msg);
  free(html);
}

static void test_feed_across_line_ending(test_batch_runner *runner) {
  // See #117
  cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
  cmark_parser_feed(parser, "line1\r", 6);
  cmark_parser_feed(parser, "\nline2\r\n", 8);
  cmark_node *document = cmark_parser_finish(parser);
  OK(runner, document->first_child->next == NULL, "document has one paragraph");
  cmark_parser_free(parser);
  cmark_node_free(document);
}

static char *render_oneshot_xml(const char *input, size_t len, int gfm) {
  char *xml;

  if (!gfm) {
    cmark_node *doc = cmark_parse_document(input, len, CMARK_OPT_DEFAULT);
    xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT);
    cmark_node_free(doc);
    return xml;
  }

  cmark_gfm_core_extensions_ensure_registered();
  cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
  attach_gfm_core_extensions(parser);
  cmark_parser_feed(parser, input, len);
  cmark_node *doc = cmark_parser_finish(parser);
  xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT);
  cmark_node_free(doc);
  cmark_parser_free(parser);
  return xml;
}

static void assert_snapshot_matches_oneshot(test_batch_runner *runner,
                                            const char *input,
                                            int gfm,
                                            const char *msg) {
  size_t len = strlen(input);
  cmark_parser *parser = cmark_parser_new(CMARK_OPT_FEED_AST);
  if (gfm) {
    cmark_gfm_core_extensions_ensure_registered();
    attach_gfm_core_extensions(parser);
  }

  cmark_parser_feed(parser, input, len);
  cmark_node *snap = cmark_parser_snapshot(parser);
  char *xml_snap = cmark_render_xml(snap, CMARK_OPT_DEFAULT);
  char *xml_one = render_oneshot_xml(input, len, gfm);

  STR_EQ(runner, xml_snap, xml_one, "%s XML", msg);

  cmark_node *doc = cmark_parser_finish(parser);
  cmark_node_free(doc);
  cmark_parser_free(parser);
  free(xml_snap);
  free(xml_one);
}

static void assert_split_snapshot_matches_oneshot(test_batch_runner *runner,
                                                  const char *first,
                                                  const char *second,
                                                  int options,
                                                  int gfm,
                                                  const char *msg) {
  size_t first_len = strlen(first);
  size_t second_len = strlen(second);
  char *input = (char *)malloc(first_len + second_len + 1);
  memcpy(input, first, first_len);
  memcpy(input + first_len, second, second_len);
  input[first_len + second_len] = '\0';

  cmark_parser *parser = cmark_parser_new(CMARK_OPT_FEED_AST | options);
  if (gfm) {
    cmark_gfm_core_extensions_ensure_registered();
    attach_gfm_core_extensions(parser);
  }
  cmark_parser_feed(parser, first, first_len);
  cmark_parser_snapshot(parser);
  cmark_parser_feed(parser, second, second_len);
  cmark_node *snap = cmark_parser_snapshot(parser);
  char *xml_snap = cmark_render_xml(snap, options);

  cmark_node *one;
  if (gfm) {
    cmark_parser *one_parser = cmark_parser_new(options);
    attach_gfm_core_extensions(one_parser);
    cmark_parser_feed(one_parser, input, first_len + second_len);
    one = cmark_parser_finish(one_parser);
    cmark_parser_free(one_parser);
  } else {
    one = cmark_parse_document(input, first_len + second_len, options);
  }
  char *xml_one = cmark_render_xml(one, options);

  STR_EQ(runner, xml_snap, xml_one, "%s", msg);

  cmark_node *doc = cmark_parser_finish(parser);
  cmark_node_free(doc);
  cmark_parser_free(parser);
  cmark_node_free(one);
  free(xml_snap);
  free(xml_one);
  free(input);
}

static void assert_feed_prefixes_match(test_batch_runner *runner,
                                            const char *input,
                                            size_t case_index,
                                            int gfm,
                                            const char *label) {
  size_t input_len = strlen(input);
  cmark_parser *parser = cmark_parser_new(CMARK_OPT_FEED_AST);
  if (gfm) {
    cmark_gfm_core_extensions_ensure_registered();
    attach_gfm_core_extensions(parser);
  }

  for (size_t k = 0; k < input_len; ++k) {
    cmark_parser_feed(parser, input + k, 1);
    cmark_node *snap = cmark_parser_snapshot(parser);
    char *xml_snap = cmark_render_xml(snap, CMARK_OPT_DEFAULT);
    char *xml_one = render_oneshot_xml(input, k + 1, gfm);

    // XML pins the observable AST contract, including adjacent TEXT-node
    // consolidation. If the AST matches cmark_parse_document(prefix, ...),
    // rendering with the same options/extensions is deterministic.
    STR_EQ(runner, xml_snap, xml_one,
           "%s snapshot-convergence[%zu] prefix %zu XML", label, case_index,
           k + 1);

    free(xml_snap);
    free(xml_one);
  }

  cmark_node *streamed = cmark_parser_finish(parser);
  char *xml_stream = cmark_render_xml(streamed, CMARK_OPT_DEFAULT);
  char *xml_one = render_oneshot_xml(input, input_len, gfm);

  STR_EQ(runner, xml_stream, xml_one,
         "%s final-convergence[%zu] XML",
         label, case_index);

  free(xml_one);
  free(xml_stream);
  cmark_node_free(streamed);
  cmark_parser_free(parser);
}

typedef struct {
  uint32_t magic;
  size_t size;
} tracked_alloc_header;

#define TRACKED_ALLOC_MAGIC 0x434d4152u

static size_t tracked_alloc_count = 0;
static size_t tracked_alloc_bytes = 0;

static void tracked_alloc_reset(void) {
  tracked_alloc_count = 0;
  tracked_alloc_bytes = 0;
}

static void *tracked_calloc(size_t nmemb, size_t size) {
  if (nmemb && size > (SIZE_MAX - sizeof(tracked_alloc_header)) / nmemb) {
    return NULL;
  }
  size_t user_size = nmemb * size;
  tracked_alloc_header *header =
      (tracked_alloc_header *)calloc(1, sizeof(*header) + user_size);
  if (!header) {
    return NULL;
  }
  header->magic = TRACKED_ALLOC_MAGIC;
  header->size = user_size;
  tracked_alloc_count++;
  tracked_alloc_bytes += user_size;
  return header + 1;
}

static void *tracked_realloc(void *ptr, size_t size) {
  if (!ptr) {
    return tracked_calloc(1, size);
  }
  if (size == 0) {
    tracked_alloc_header *header = ((tracked_alloc_header *)ptr) - 1;
    if (header->magic == TRACKED_ALLOC_MAGIC) {
      tracked_alloc_count--;
      tracked_alloc_bytes -= header->size;
      header->magic = 0;
    }
    free(header);
    return NULL;
  }

  tracked_alloc_header *header = ((tracked_alloc_header *)ptr) - 1;
  if (header->magic != TRACKED_ALLOC_MAGIC) {
    abort();
  }
  tracked_alloc_header *resized =
      (tracked_alloc_header *)realloc(header, sizeof(*header) + size);
  if (!resized) {
    return NULL;
  }
  tracked_alloc_bytes -= resized->size;
  resized->magic = TRACKED_ALLOC_MAGIC;
  resized->size = size;
  tracked_alloc_bytes += size;
  return resized + 1;
}

static void tracked_free(void *ptr) {
  if (!ptr) {
    return;
  }
  tracked_alloc_header *header = ((tracked_alloc_header *)ptr) - 1;
  if (header->magic != TRACKED_ALLOC_MAGIC) {
    abort();
  }
  tracked_alloc_count--;
  tracked_alloc_bytes -= header->size;
  header->magic = 0;
  free(header);
}

// Feed-driven AST tests — cover snapshot semantics, partial-line
// convergence, and the internal regressions that protect those guarantees.
static void test_feed_ast(test_batch_runner *runner) {
  // Precondition: cmark_parser_snapshot is only valid on parsers created
  // with CMARK_OPT_FEED_AST. Without the flag the dirty-blocks list,
  // partial-line txn, and pending-ref index are never populated, so any
  // tree we returned would be missing inline children for committed
  // paragraphs and miss the partial-line tentative pass. Returning NULL
  // surfaces the misuse explicitly rather than silently producing a wrong
  // tree — feed mode must be requested at parser creation, not implicitly
  // activated by calling snapshot.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_DEFAULT);
    cmark_parser_feed(p, "Hello\n", 6);
    cmark_node *snap = cmark_parser_snapshot(p);
    OK(runner, snap == NULL,
       "snapshot without CMARK_OPT_FEED_AST returns NULL");
    cmark_node *doc = cmark_parser_finish(p);
    OK(runner, doc != NULL,
       "feed+finish on a non-feed-mode parser still produces a document");
    cmark_node_free(doc);
    cmark_parser_free(p);
  }

  // Snapshot returns the live root and is non-NULL even before finish.
  // Partial line converges: feed("Hello", 5); snapshot() must match a
  // one-shot parse of "Hello".
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "Hello", 5);
    cmark_node *snap = cmark_parser_snapshot(p);
    OK(runner, snap != NULL, "snapshot returns non-null root before finish");
    INT_EQ(runner, (int)cmark_node_get_type(snap), (int)CMARK_NODE_DOCUMENT,
           "snapshot root is document");
    cmark_node *para = cmark_node_first_child(snap);
    OK(runner, para != NULL,
       "snapshot of partial line shows the in-flight paragraph");
    if (para) {
      INT_EQ(runner, (int)cmark_node_get_type(para),
             (int)CMARK_NODE_PARAGRAPH,
             "partial-line snapshot contains a paragraph");
      cmark_node *txt = cmark_node_first_child(para);
      OK(runner, txt && cmark_node_get_type(txt) == CMARK_NODE_TEXT &&
                 strcmp(cmark_node_get_literal(txt), "Hello") == 0,
         "partial-line paragraph holds the inline text 'Hello'");
    }
    cmark_parser_free(p);
  }

  // Snapshot convergence applies to every byte prefix, not just to the final
  // tree after finish(). These cases pin the prefixes that previously drifted
  // from cmark_parse_document(prefix, ...).
  assert_snapshot_matches_oneshot(
      runner, "Hello\n=", 0,
      "snapshot setext-prefix keeps paragraph text in promoted heading");
  assert_snapshot_matches_oneshot(
      runner, "[foo]: h", 0,
      "snapshot EOF-style finalizes a reference-only paragraph");
  assert_snapshot_matches_oneshot(
      runner, "See [foo].\n\n[foo]: h", 0,
      "snapshot resolves an earlier reference when a trailing definition arrives");
  assert_snapshot_matches_oneshot(
      runner, "```c", 0,
      "snapshot EOF-style finalizes fenced-code info strings");
  assert_snapshot_matches_oneshot(
      runner, "- [", 1,
      "GFM snapshot matches one-shot list-item paragraph tightness");
  assert_snapshot_matches_oneshot(
      runner, "A | B\n-", 1,
      "GFM snapshot setext/table-prefix keeps promoted heading text");

  // Block identity is part of the feed-mode contract and is the public
  // signal that snapshots are incremental rather than whole-tree rebuilds.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "Hello\n", 6);
    cmark_node *snap1 = cmark_parser_snapshot(p);
    cmark_node *block = cmark_node_first_child(snap1);
    OK(runner, block != NULL &&
               cmark_node_get_type(block) == CMARK_NODE_PARAGRAPH,
       "setext identity: initial block is a paragraph");

    cmark_parser_feed(p, "=", 1);
    cmark_node *snap2 = cmark_parser_snapshot(p);
    cmark_node *heading = cmark_node_first_child(snap2);
    OK(runner, heading == block,
       "setext identity: paragraph morphs in place to heading");
    OK(runner, heading != NULL &&
               cmark_node_get_type(heading) == CMARK_NODE_HEADING,
       "setext identity: morphed block is a heading");
    cmark_node *txt = cmark_node_first_child(heading);
    OK(runner, txt && cmark_node_get_type(txt) == CMARK_NODE_TEXT &&
               strcmp(cmark_node_get_literal(txt), "Hello") == 0,
       "setext identity: morphed heading keeps inline text");

    cmark_node *doc = cmark_parser_finish(p);
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  {
    cmark_gfm_core_extensions_ensure_registered();
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    attach_gfm_core_extensions(p);
    cmark_parser_feed(p, "A | B\n", 6);
    cmark_node *snap1 = cmark_parser_snapshot(p);
    cmark_node *block = cmark_node_first_child(snap1);
    OK(runner, block != NULL &&
               cmark_node_get_type(block) == CMARK_NODE_PARAGRAPH,
       "table identity: initial block is a paragraph");

    cmark_parser_feed(p, "--- | ---", 9);
    cmark_node *snap2 = cmark_parser_snapshot(p);
    cmark_node *table = cmark_node_first_child(snap2);
    OK(runner, table == block,
       "table identity: paragraph morphs in place to table");
    OK(runner, table != NULL &&
               strcmp(cmark_node_get_type_string(table), "table") == 0,
       "table identity: morphed block is a table");
    OK(runner, cmark_node_first_child(table) != NULL,
       "table identity: morphed table has header row children");

    cmark_node *doc = cmark_parser_finish(p);
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "one\n\ntwo\n\n", 10);
    cmark_node *snap1 = cmark_parser_snapshot(p);
    cmark_node *first = cmark_node_first_child(snap1);
    cmark_node *second = first ? cmark_node_next(first) : NULL;
    OK(runner, first != NULL && second != NULL,
       "block identity: two closed paragraphs exist before later input");

    cmark_parser_feed(p, "three", 5);
    cmark_node *snap2 = cmark_parser_snapshot(p);
    OK(runner, cmark_node_first_child(snap2) == first,
       "block identity: first closed block survives later snapshot");
    OK(runner, first && cmark_node_next(first) == second,
       "block identity: second closed block survives later snapshot");

    cmark_node *doc = cmark_parser_finish(p);
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  // Continuation: feeding more bytes that complete the line must not
  // double-count the partial-line content. The streamed final tree must
  // match cmark_parse_document on the same byte sequence.
  {
    const char *input = "Hello, world\n";
    size_t len = strlen(input);
    cmark_node *one = cmark_parse_document(input, len, CMARK_OPT_DEFAULT);
    char *xml_one = cmark_render_xml(one, CMARK_OPT_DEFAULT);
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "Hello", 5);
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, ", world\n", 8);
    cmark_node *streamed = cmark_parser_finish(p);
    char *xml_stream = cmark_render_xml(streamed, CMARK_OPT_DEFAULT);
    STR_EQ(runner, xml_stream, xml_one,
           "partial-line snapshot followed by line completion converges XML");
    free(xml_one); free(xml_stream);
    cmark_node_free(one); cmark_node_free(streamed);
    cmark_parser_free(p);
  }

  // Snapshot is parser-owned and remains valid until the next feed/snapshot/
  // finish; calling finish after a snapshot still yields a usable tree.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "Hello\n", 6);
    cmark_node *snap = cmark_parser_snapshot(p);
    OK(runner, snap != NULL, "snapshot is non-null");
    cmark_node *doc = cmark_parser_finish(p);
    OK(runner, doc != NULL, "finish after snapshot returns a tree");
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  // Reverting a partial-line transaction after a snapshot with inline
  // children must not write flag snapshots back into freed inline nodes.
  // Normal builds exercise the sequence; ASan/UBSan builds catch the UAF.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "Hello\n", 6);
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, "=", 1);
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, "=", 1);
    cmark_node *doc = cmark_parser_finish(p);
    OK(runner, doc != NULL,
       "partial-line revert after parsed inlines does not hit freed nodes");
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  // Snapshot returns a tree with inline children populated. A closed
  // emphasis becomes EMPH; an unclosed delimiter falls back to literal text.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "Hello *world*\n", 14);
    cmark_node *snap = cmark_parser_snapshot(p);
    cmark_node *para = cmark_node_first_child(snap);
    OK(runner, para != NULL && cmark_node_get_type(para) == CMARK_NODE_PARAGRAPH,
       "first child is a paragraph");
    cmark_node *first_inline = cmark_node_first_child(para);
    OK(runner, first_inline != NULL,
       "snapshot populated inline children");
    int saw_emph = 0;
    for (cmark_node *c = first_inline; c; c = cmark_node_next(c)) {
      if (cmark_node_get_type(c) == CMARK_NODE_EMPH)
        saw_emph = 1;
    }
    OK(runner, saw_emph,
       "closed *...* surfaces as CMARK_NODE_EMPH after snapshot");
    cmark_node *doc = cmark_parser_finish(p);
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  // When a [foo] reference is parsed before its definition arrives, the
  // containing paragraph gets re-parsed once the definition is added.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "See [foo] for details\n\n", 23);
    cmark_node *snap1 = cmark_parser_snapshot(p);
    cmark_node *para = cmark_node_first_child(snap1);
    OK(runner, para != NULL, "paragraph created");
    int link_before = 0;
    for (cmark_node *c = cmark_node_first_child(para); c;
         c = cmark_node_next(c)) {
      if (cmark_node_get_type(c) == CMARK_NODE_LINK) link_before = 1;
    }
    OK(runner, !link_before,
       "[foo] is plain text before definition arrives");

    cmark_parser_feed(p, "[foo]: http://example.com\n\n", 27);
    cmark_node *snap2 = cmark_parser_snapshot(p);
    cmark_node *para2 = cmark_node_first_child(snap2);
    OK(runner, para2 == para,
       "ref-resolved paragraph keeps pointer identity");
    int link_after = 0;
    const char *link_url = NULL;
    for (cmark_node *c = cmark_node_first_child(para2); c;
         c = cmark_node_next(c)) {
      if (cmark_node_get_type(c) == CMARK_NODE_LINK) {
        link_after = 1;
        link_url = cmark_node_get_url(c);
      }
    }
    OK(runner, link_after,
       "[foo] re-parses as CMARK_NODE_LINK after definition arrives");
    OK(runner, link_url && strcmp(link_url, "http://example.com") == 0,
       "resolved link carries the correct URL");
    cmark_node *doc = cmark_parser_finish(p);
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  // S_finalize frees a ref-def-only paragraph that may still be on the
  // dirty-blocks list (add_line marks every block dirty on append; we do
  // not require a snapshot before the next feed). The next snapshot must
  // not walk into freed memory.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "[foo]: http://example.com\n", 26);
    cmark_parser_feed(p, "\n", 1); // close → paragraph freed
    cmark_node *snap = cmark_parser_snapshot(p);
    OK(runner, snap != NULL,
       "snapshot survives a freed dirty paragraph (UAF regression)");
    cmark_node *doc = cmark_parser_finish(p);
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  // A tentative ref-def with a possible title continuation exercises the
  // dirty-list revert path: the partial-line snapshot marks the paragraph
  // dirty, the next feed reverts flags, then appends canonical bytes. The
  // dirty list must never retain the same node twice or form a self-loop;
  // otherwise the next snapshot can reprocess the block through stale list
  // links or, after ref-def-only finalize frees it, walk freed memory.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    const char *def_line = "[foo]: /url\n";
    const char *title = "   \"title\"";

    cmark_parser_feed(p, def_line, strlen(def_line));
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, title, strlen(title));
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, "\n", 1);

    cmark_node *dirty = p->feed.dirty_blocks_head;
    OK(runner, dirty == NULL || dirty->dirty_next != dirty,
       "dirty list has no self-loop after tentative ref-def title revert");
    OK(runner, dirty == NULL ||
               (dirty->flags & CMARK_NODE__INLINE_DIRTY) != 0,
       "dirty list entries keep INLINE_DIRTY set after tentative revert");

    cmark_node *doc = cmark_parser_finish(p);
    cmark_node_free(doc);
    cmark_parser_free(p);
  }

  // process_inlines walks the tree with cmark_iter; the iterator caches
  // first_child eagerly. If a snapshot-parsed block is later re-marked
  // INLINE_DIRTY by ref resolution at finalize, the !already_parsed branch
  // frees those children before the iterator advances. ASan/UBSan catch
  // the dangling pointer; behavioral check that streamed XML matches
  // one-shot also exercises the path.
  {
    const char *streamed_a = "See [foo].\n\n";
    const char *streamed_b = "[foo]: http://example.com\n";
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s", streamed_a, streamed_b);
    char *expected = render_oneshot_xml(buf, strlen(buf), 0);

    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, streamed_a, strlen(streamed_a));
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, streamed_b, strlen(streamed_b));
    cmark_node *doc = cmark_parser_finish(p);
    char *xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT);
    STR_EQ(runner, xml, expected,
           "ref-resolved-at-finish streamed XML matches one-shot XML");
    free(xml); free(expected);
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  // cmark_parser_feed_reentrant injects bytes that are part of the document
  // tree but must not corrupt the partial-line buffer of the outer feed.
  // After "partial" + reentrant("complete\n\n") + " more\n\n", the linebuf
  // for "partial" must still terminate cleanly. End-to-end check: finish
  // returns without crashing and the resulting AST matches one-shot parse
  // of the effective document order.
  {
    const char *expected_input = "complete\n\npartial more\n\n";
    char *expected = render_oneshot_xml(expected_input,
                                        strlen(expected_input), 0);
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "partial", 7);
    cmark_parser_feed_reentrant(p, "complete\n\n", 10);
    cmark_parser_feed(p, " more\n\n", 7);
    cmark_node *doc = cmark_parser_finish(p);
    OK(runner, doc != NULL, "feed_reentrant: finish returns a document");
    char *xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT);
    STR_EQ(runner, xml, expected,
           "feed_reentrant streamed XML matches effective one-shot XML");
    free(xml);
    free(expected);
    cmark_node_free(doc);
    cmark_parser_free(p);
  }

  // Same as above, but with a live snapshot before the reentrant feed.
  // feed_reentrant must first leave tentative snapshot state, otherwise the
  // injected blocks are treated as tentative and get lost on the next feed.
  {
    const char *expected_input = "complete\n\npartial more\n\n";
    char *expected = render_oneshot_xml(expected_input,
                                        strlen(expected_input), 0);
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "partial", 7);
    cmark_parser_snapshot(p);
    cmark_parser_feed_reentrant(p, "complete\n\n", 10);
    cmark_parser_feed(p, " more\n\n", 7);
    cmark_node *doc = cmark_parser_finish(p);
    OK(runner, doc != NULL,
       "feed_reentrant after snapshot returns a document");
    char *xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT);
    STR_EQ(runner, xml, expected,
           "feed_reentrant after snapshot streamed XML matches effective "
           "one-shot XML");
    free(xml);
    free(expected);
    cmark_node_free(doc);
    cmark_parser_free(p);
  }

  // Feed bookkeeping must be quiescent in non-feed mode:
  // cmark_parse_document and feed-only flows shouldn’t accumulate a dirty
  // list. Otherwise plain parses pay overhead they should not.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_DEFAULT);
    const char *input =
        "First paragraph.\n\n"
        "Second.\n\n"
        "- list item 1\n"
        "- list item 2\n\n";
    cmark_parser_feed(p, input, strlen(input));
    OK(runner, p->feed.dirty_blocks_head == NULL,
       "dirty list empty in non-feed mode");
    cmark_node *doc = cmark_parser_finish(p);
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  // Re-parsing the same dirty block while [foo] is still unresolved must
  // not accumulate duplicate (block, label) entries in pending_ref_users.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "See [foo]\n", 10);
    cmark_parser_snapshot(p);
    for (int i = 0; i < 30; ++i) {
      cmark_parser_feed(p, "more\n", 5);
      cmark_parser_snapshot(p);
    }
    int list_len = 0;
    for (struct cmark_pending_ref_user *u =
             p->feed.pending_ref_users_head; u; u = u->next) {
      list_len++;
    }
    INT_EQ(runner, list_len, 1,
           "pending_ref_users list stays bounded across re-parses");
    cmark_node *doc = cmark_parser_finish(p);
    cmark_parser_free(p);
    cmark_node_free(doc);
  }

  // Tentative table morph allocates extension-owned opaque payloads. Revert
  // must free those payloads before restoring the old `as` union bytes.
  {
    cmark_mem mem = {tracked_calloc, tracked_realloc, tracked_free};
    tracked_alloc_reset();
    cmark_gfm_core_extensions_ensure_registered();
    cmark_parser *p = cmark_parser_new_with_mem(CMARK_OPT_FEED_AST, &mem);
    attach_gfm_core_extensions(p);
    cmark_parser_feed(p, "A | B\n", 6);
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, "--- | ---", 9);
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, "\n", 1);
    cmark_node *doc = cmark_parser_finish(p);
    cmark_node_free(doc);
    cmark_parser_free(p);
    OK(runner, tracked_alloc_count == 0,
       "tentative table morph frees extension opaque allocations "
       "(remaining allocations: %zu, bytes: %zu)",
       tracked_alloc_count, tracked_alloc_bytes);
  }

  // Snapshot idempotency: back-to-back snapshots without an intervening
  // feed must (a) return the same tree, (b) not re-do the O(tree)
  // tentative pass. We verify (a) structurally by comparing XML and (b)
  // by checking that the underlying txn struct is the SAME pointer — a
  // rebuild would have freed and re-allocated a fresh txn.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "Para with [foo] and ", 20);
    cmark_node *snap1 = cmark_parser_snapshot(p);
    char *xml1 = cmark_render_xml(snap1, CMARK_OPT_DEFAULT);
    void *txn_after_first = (void *)p->feed.partial_txn;
    OK(runner, txn_after_first != NULL,
       "first snapshot leaves tentative txn open");

    cmark_node *snap2 = cmark_parser_snapshot(p);
    char *xml2 = cmark_render_xml(snap2, CMARK_OPT_DEFAULT);
    void *txn_after_second = (void *)p->feed.partial_txn;

    STR_EQ(runner, xml2, xml1,
           "back-to-back snapshot returns identical XML");
    OK(runner, txn_after_second == txn_after_first,
       "back-to-back snapshot reuses prior tentative txn (no rebuild)");

    // After feed, idempotency must invalidate. Next snapshot does work,
    // visible as a new txn allocation reflecting the new content.
    cmark_parser_feed(p, "more.\n", 6);
    cmark_node *snap3 = cmark_parser_snapshot(p);
    char *xml3 = cmark_render_xml(snap3, CMARK_OPT_DEFAULT);
    OK(runner, strcmp(xml3, xml1) != 0,
       "snapshot after feed reflects new content");

    free(xml1);
    free(xml2);
    free(xml3);
    cmark_node *doc = cmark_parser_finish(p);
    cmark_node_free(doc);
    cmark_parser_free(p);
  }

  // Oneshot purity: when snapshot is never called, feed bookkeeping
  // must remain quiescent. The oracle for feed-mode correctness is
  // cmark_parse_document; if any feed-only mutation runs in oneshot
  // the oracle is contaminated.
  {
    // Pure feed+finish (no OPT_FEED_AST, no snapshot) must leave feed
    // state empty. This includes the paths most likely to incidentally
    // touch feed-mode bookkeeping: ref-def extraction (try_eager_ref_extract
    // gating), pending-ref registration (gated on OPT_FEED_AST), and
    // tentative txn (only created by snapshot). Use a "use-before-def"
    // document so pending-ref bookkeeping would fire if it were not gated.
    cmark_parser *p = cmark_parser_new(CMARK_OPT_DEFAULT);
    const char *part1 = "See [x] here.\n\n";
    const char *part2 = "[x]: http://x.example\n";
    cmark_parser_feed(p, part1, strlen(part1));
    cmark_parser_feed(p, part2, strlen(part2));
    OK(runner, p->feed.dirty_blocks_head == NULL,
       "feed without snapshot does not enqueue dirty blocks");
    OK(runner, p->feed.partial_txn == NULL,
       "feed without snapshot does not allocate a partial txn");
    OK(runner, p->feed.pending_ref_users_head == NULL,
       "feed without snapshot does not register pending-ref users");
    cmark_node *doc = cmark_parser_finish(p);
    OK(runner, doc != NULL, "feed+finish returns a document");

    // Sanity vs. oracle: feed+finish output must be byte-identical to a
    // single cmark_parse_document of the concatenated input. The "use-
    // before-def" arrangement exercises the failure mode that would
    // surface if feed bookkeeping had silently been doing something
    // here — eager ref extract, pending-ref registration, snapshot
    // tentative — any of which would change observable XML.
    char *xml_streamed = cmark_render_xml(doc, CMARK_OPT_DEFAULT);
    char concat[64];
    snprintf(concat, sizeof(concat), "%s%s", part1, part2);
    cmark_node *oracle = cmark_parse_document(concat, strlen(concat),
                                              CMARK_OPT_DEFAULT);
    char *xml_oracle = cmark_render_xml(oracle, CMARK_OPT_DEFAULT);
    STR_EQ(runner, xml_streamed, xml_oracle,
           "feed+finish output identical to cmark_parse_document oracle");
    free(xml_streamed);
    free(xml_oracle);
    cmark_node_free(doc);
    cmark_node_free(oracle);
    cmark_parser_free(p);
  }

  // Inline incremental resume — when an open paragraph grows by appending a
  // continuation line and the prior parse left the delim/bracket stacks
  // empty (CLEAN_END), the canonical dirty drain reuses prior children
  // and parses only the delta. We assert it's actually incremental by
  // pinning the FIRST text node's pointer identity across the resume —
  // a full reparse would free and recreate it.
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    // First feed completes a line so the paragraph block is materialized
    // in canonical state with content "Hello\n".
    cmark_parser_feed(p, "Hello\n", 6);
    cmark_node *snap1 = cmark_parser_snapshot(p);
    cmark_node *para = cmark_node_first_child(snap1);
    OK(runner, para != NULL && cmark_node_get_type(para) == CMARK_NODE_PARAGRAPH,
       "incremental: snap1 has a paragraph");
    cmark_node *text_first_before = cmark_node_first_child(para);
    OK(runner, text_first_before != NULL &&
               cmark_node_get_type(text_first_before) == CMARK_NODE_TEXT &&
               strcmp(cmark_node_get_literal(text_first_before), "Hello") == 0,
       "incremental: first child is text 'Hello'");
    OK(runner, (para->flags & CMARK_NODE__INLINE_CLEAN_END) != 0,
       "incremental: paragraph parse marked CLEAN_END");

    // Append a continuation line that adds a softbreak + new text run.
    // The canonical dirty drain on the next snapshot must take the resume
    // path; the first text node must be the same pointer afterwards.
    cmark_parser_feed(p, " world\n", 7);
    cmark_node *snap2 = cmark_parser_snapshot(p);
    cmark_node *para2 = cmark_node_first_child(snap2);
    OK(runner, para2 == para,
       "incremental: paragraph pointer identity preserved");
    cmark_node *text_first_after = cmark_node_first_child(para2);
    OK(runner, text_first_after == text_first_before,
       "incremental: first text node pointer preserved across resume "
       "(would be freed by a full reparse)");

    // Structural verification — the resume must produce the same tree as
    // a fresh oneshot parse of the concatenated input.
    char *xml_streamed = cmark_render_xml(snap2, CMARK_OPT_DEFAULT);
    char *xml_oracle = render_oneshot_xml("Hello\n world\n", 13, 0);
    STR_EQ(runner, xml_streamed, xml_oracle,
           "incremental resume converges with oneshot for multi-line paragraph");
    free(xml_streamed);
    free(xml_oracle);

    cmark_node *doc = cmark_parser_finish(p);
    cmark_node_free(doc);
    cmark_parser_free(p);
  }

  // Resume must preserve SOURCEPOS across already-parsed prefixes. Skipping
  // to start_offset without replaying newlines in the skipped prefix makes
  // later children inherit the wrong subject line/column.
  {
    const char *input = "a\nb\nc\n";
    cmark_node *oracle = cmark_parse_document(input, strlen(input),
                                              CMARK_OPT_SOURCEPOS);
    char *xml_oracle = cmark_render_xml(oracle, CMARK_OPT_SOURCEPOS);

    cmark_parser *p =
        cmark_parser_new(CMARK_OPT_FEED_AST | CMARK_OPT_SOURCEPOS);
    cmark_parser_feed(p, "a\nb\n", 4);
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, "c\n", 2);
    cmark_node *snap = cmark_parser_snapshot(p);
    char *xml_streamed = cmark_render_xml(snap, CMARK_OPT_SOURCEPOS);

    STR_EQ(runner, xml_streamed, xml_oracle,
           "incremental inline resume preserves sourcepos across prefixes");

    free(xml_streamed);
    free(xml_oracle);
    cmark_node_free(oracle);
    cmark_node *doc = cmark_parser_finish(p);
    cmark_node_free(doc);
    cmark_parser_free(p);
  }

  // Split-feed inline coverage: these cases all parse the first chunk into
  // canonical inline children, then append more bytes to the same paragraph.
  // Any syntax that can still reach back across that boundary must force a
  // full reparse rather than append-only resume.
  assert_split_snapshot_matches_oneshot(
      runner, "[x\n", "](u)\n", CMARK_OPT_DEFAULT, 0,
      "open link text across snapshots converges with oneshot");
  assert_split_snapshot_matches_oneshot(
      runner, "\"hello\n", "\"\n", CMARK_OPT_SMART, 0,
      "smart quote delimiters across snapshots converge with oneshot");
  assert_split_snapshot_matches_oneshot(
      runner, "~~gone\n", "now~~\n", CMARK_OPT_DEFAULT, 1,
      "GFM strikethrough delimiters across snapshots converge with oneshot");
  assert_split_snapshot_matches_oneshot(
      runner, "<a\n", "href=x>\n", CMARK_OPT_DEFAULT, 0,
      "open HTML tag with multiline attributes converges with oneshot");
  assert_split_snapshot_matches_oneshot(
      runner, "[x](u\n", ")\n", CMARK_OPT_DEFAULT, 0,
      "inline link destination across snapshots converges with oneshot");
  assert_split_snapshot_matches_oneshot(
      runner, "[x](u\n", "\"title\")\n", CMARK_OPT_DEFAULT, 0,
      "inline link title across snapshots converges with oneshot");
  assert_split_snapshot_matches_oneshot(
      runner, "![x](u\n", ")\n", CMARK_OPT_DEFAULT, 0,
      "inline image destination across snapshots converges with oneshot");
  assert_split_snapshot_matches_oneshot(
      runner, "Email ", "a@b.com\n", CMARK_OPT_DEFAULT, 1,
      "GFM email autolink postprocess is visible in snapshots");

  // Resume must NOT fire when the prior parse left an unclosed code span.
  // A closing backtick in later input can reach back across the already-
  // parsed prefix and rewrite literal text into a CODE node, so this case
  // must fall back to a full reparse instead of append-only resume.
  {
    const char *input = "`code\n`\n";
    char *xml_oracle = render_oneshot_xml(input, strlen(input), 0);

    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "`code\n", 6);
    cmark_parser_snapshot(p);
    cmark_parser_feed(p, "`\n", 2);
    cmark_node *snap = cmark_parser_snapshot(p);
    char *xml_streamed = cmark_render_xml(snap, CMARK_OPT_DEFAULT);

    STR_EQ(runner, xml_streamed, xml_oracle,
           "open code span across snapshots converges with oneshot");

    free(xml_streamed);
    free(xml_oracle);
    cmark_node *doc = cmark_parser_finish(p);
    cmark_node_free(doc);
    cmark_parser_free(p);
  }

  // Resume must NOT fire when the prior parse left an open delimiter —
  // a closing `*` in the new content can pair with the prefix's `*`
  // and reorganize earlier inlines into an emph node, which is not
  // expressible as an append. The CLEAN_END flag should be clear and
  // the dirty drain must take the full-reparse path. We verify by the
  // first text node being a NEW pointer post-feed (the old one was
  // freed by full reparse).
  {
    cmark_parser *p = cmark_parser_new(CMARK_OPT_FEED_AST);
    cmark_parser_feed(p, "open *star\n", 11);
    cmark_node *snap1 = cmark_parser_snapshot(p);
    cmark_node *para = cmark_node_first_child(snap1);
    OK(runner, para != NULL && (para->flags & CMARK_NODE__INLINE_CLEAN_END) == 0,
       "open `*` leaves stacks dirty — CLEAN_END not set");

    cmark_parser_feed(p, "now closed*\n", 12);
    cmark_node *snap2 = cmark_parser_snapshot(p);
    char *xml_streamed = cmark_render_xml(snap2, CMARK_OPT_DEFAULT);
    char *xml_oracle = render_oneshot_xml("open *star\nnow closed*\n", 23, 0);
    STR_EQ(runner, xml_streamed, xml_oracle,
           "open-emph case full-reparses correctly to oracle");
    OK(runner, strstr(xml_streamed, "<emph>") != NULL,
       "open-emph case: closing `*` does pair into <emph>");
    free(xml_streamed);
    free(xml_oracle);

    cmark_node *doc = cmark_parser_finish(p);
    cmark_node_free(doc);
    cmark_parser_free(p);
  }
}

// Differential validation: for a representative corpus of markdown inputs,
// confirm that feed parse with snapshots-between-every-byte produces
// the same AST/XML as one-shot parse for every prefix and final document.
static void test_feed_convergence(test_batch_runner *runner) {
  static const char *corpus[] = {
    "Hello world\n",
    "Hello\n=====\n",
    "# Heading\n\nParagraph with *emphasis* and **strong**.\n",
    "- item one\n- item two\n- item three\n",
    "1. one\n2. two\n\n3. three (loose)\n",
    "> blockquote line one\n> line two\n",
    "```c\nint main(void){return 0;}\n```\n",
    "    indented code\n    second line\n",
    "Header 1 | Header 2\n--- | ---\nA | B\nC | D\n",
    "See [foo] for more.\n\n[foo]: http://example.com \"title\"\n",
    "First paragraph.\n\nSecond with `code` and a [link](http://x.com).\n",
    "<p>raw html</p>\n\nThen prose.\n",
    "Paragraph one.\nLine two of paragraph one.\n\nParagraph two.\n",
    "Mix: **bold *nested italic* still bold** plain\n",
    // Direction 3 edge cases:
    //  - title-on-continuation-line. Eager extract MUST defer until full
    //    title arrives, otherwise streamed-vs-oneshot diverges.
    "[foo]: http://x.example\n   \"title here\"\n\nUse [foo].\n",
    //  - multiple back-to-back defs. The first is provably bounded by the
    //    second `[`, so eager extract should fire.
    "[a]: http://a.example\n[b]: http://b.example\n\nSee [a] and [b].\n",
    //  - def followed by non-title non-whitespace prose.
    "[a]: http://a.example\nNot a title line.\n\nUsing [a].\n",
    //  - unclosed emphasis spanning multiple lines, then closed.
    "Open *star\nstill open\nnow closed*\n",
  };
  size_t n = sizeof(corpus) / sizeof(*corpus);

  for (size_t i = 0; i < n; ++i) {
    assert_feed_prefixes_match(runner, corpus[i], i, 0,
                                    "commonmark");
  }

  // Same convergence guarantee with GFM extensions enabled — exercises the
  // paragraph -> table morph and the strikethrough/autolink inline paths.
  cmark_gfm_core_extensions_ensure_registered();
  static const char *gfm_corpus[] = {
    "Header 1 | Header 2\n--- | ---\nA | B\nC | D\n",
    "A | B\n--- | ---\nhttps://github.com | www.github.com\n",
    "Mix ~strike~ and **bold** text.\n",
    "Visit https://example.com today.\n",
    "Tasks:\n- [ ] todo\n- [x] done\n",
  };
  size_t gn = sizeof(gfm_corpus) / sizeof(*gfm_corpus);
  for (size_t i = 0; i < gn; ++i) {
    assert_feed_prefixes_match(runner, gfm_corpus[i], i, 1, "gfm");
  }

  // CMARK_OPT_FEED_AST equivalence: cmark_parse_document(input, 0)
  // and cmark_parse_document(input, OPT_FEED_AST) must produce
  // byte-identical output. The pristine path is the oracle for the
  // feed path, so any divergence at this level is a feed-path bug
  // — and it'd be invisible to the prefix-convergence tests above
  // (those test snapshot ≡ oneshot of the prefix; this tests the entire
  // pipeline through finish, which exercises tentative-revert + canonical
  // finalize on a real document, not just snapshots).
  for (size_t i = 0; i < n; ++i) {
    char *xml_pristine = render_oneshot_xml(corpus[i], strlen(corpus[i]), 0);
    cmark_node *feed_doc = cmark_parse_document(
        corpus[i], strlen(corpus[i]), CMARK_OPT_FEED_AST);
    char *xml_feed = cmark_render_xml(feed_doc, CMARK_OPT_DEFAULT);
    STR_EQ(runner, xml_feed, xml_pristine,
           "OPT_FEED_AST equivalence[%zu] commonmark", i);
    free(xml_pristine);
    free(xml_feed);
    cmark_node_free(feed_doc);
  }

  // GFM equivalence: same idea but with extensions attached. This routes
  // through cmark_parser_new + attach_gfm + feed + finish on both sides,
  // since cmark_parse_document doesn't auto-attach extensions. The
  // feed-mode parser is created with OPT_FEED_AST so its feed
  // and finish use the feed-aware path; the pristine parser uses
  // the default zero options so it stays on the oneshot path.
  for (size_t i = 0; i < gn; ++i) {
    char *xml_pristine = render_oneshot_xml(gfm_corpus[i],
                                             strlen(gfm_corpus[i]), 1);

    cmark_parser *feed_parser = cmark_parser_new(CMARK_OPT_FEED_AST);
    attach_gfm_core_extensions(feed_parser);
    cmark_parser_feed(feed_parser, gfm_corpus[i],
                      strlen(gfm_corpus[i]));
    cmark_node *feed_doc = cmark_parser_finish(feed_parser);
    char *xml_feed = cmark_render_xml(feed_doc, CMARK_OPT_DEFAULT);

    STR_EQ(runner, xml_feed, xml_pristine,
           "OPT_FEED_AST equivalence[%zu] gfm", i);

    free(xml_pristine);
    free(xml_feed);
    cmark_node_free(feed_doc);
    cmark_parser_free(feed_parser);
  }
}

#if !defined(_WIN32) || defined(__CYGWIN__)
#  include <sys/time.h>
static struct timeval _before, _after;
static int _timing;
#  define START_TIMING() \
       gettimeofday(&_before, NULL)

#  define END_TIMING() \
        do { \
          gettimeofday(&_after, NULL); \
          _timing = (_after.tv_sec - _before.tv_sec) * 1000 + (_after.tv_usec - _before.tv_usec) / 1000; \
        } while (0)

#  define TIMING _timing
#else
#  define START_TIMING()
#  define END_TIMING()
#  define TIMING 0
#endif

static void test_pathological_regressions(test_batch_runner *runner) {
  {
    // I don't care what the output is, so long as it doesn't take too long.
    char path[] = "[a](b";
    char *input = (char *)calloc(1, (sizeof(path) - 1) * 50000);
    for (int i = 0; i < 50000; ++i)
      memcpy(input + i * (sizeof(path) - 1), path, sizeof(path) - 1);

    START_TIMING();
    char *html = cmark_markdown_to_html(input, (sizeof(path) - 1) * 50000,
                                        CMARK_OPT_VALIDATE_UTF8);
    END_TIMING();
    free(html);
    free(input);

    OK(runner, TIMING < 1000, "takes less than 1000ms to run");
  }

  {
    char path[] = "[a](<b";
    char *input = (char *)calloc(1, (sizeof(path) - 1) * 50000);
    for (int i = 0; i < 50000; ++i)
      memcpy(input + i * (sizeof(path) - 1), path, sizeof(path) - 1);

    START_TIMING();
    char *html = cmark_markdown_to_html(input, (sizeof(path) - 1) * 50000,
                                        CMARK_OPT_VALIDATE_UTF8);
    END_TIMING();
    free(html);
    free(input);

    OK(runner, TIMING < 1000, "takes less than 1000ms to run");
  }
}

static void source_pos(test_batch_runner *runner) {
  static const char markdown[] =
    "# Hi *there*.\n"
    "\n"
    "Hello &ldquo; <http://www.google.com>\n"
    "there `hi` -- [okay](www.google.com (ok)).\n"
    "\n"
    "> 1. Okay.\n"
    ">    Sure.\n"
    ">\n"
    "> 2. Yes, okay.\n"
    ">    ![ok](hi \"yes\")\n";

  cmark_node *doc = cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);
  char *xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT | CMARK_OPT_SOURCEPOS);
  STR_EQ(runner, xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                      "<document sourcepos=\"1:1-10:20\" xmlns=\"http://commonmark.org/xml/1.0\">\n"
                      "  <heading sourcepos=\"1:1-1:13\" level=\"1\">\n"
                      "    <text sourcepos=\"1:3-1:5\" xml:space=\"preserve\">Hi </text>\n"
                      "    <emph sourcepos=\"1:6-1:12\">\n"
                      "      <text sourcepos=\"1:7-1:11\" xml:space=\"preserve\">there</text>\n"
                      "    </emph>\n"
                      "    <text sourcepos=\"1:13-1:13\" xml:space=\"preserve\">.</text>\n"
                      "  </heading>\n"
                      "  <paragraph sourcepos=\"3:1-4:42\">\n"
                      "    <text sourcepos=\"3:1-3:14\" xml:space=\"preserve\">Hello \xe2\x80\x9c </text>\n"
                      "    <link sourcepos=\"3:15-3:37\" destination=\"http://www.google.com\" title=\"\">\n"
                      "      <text sourcepos=\"3:16-3:36\" xml:space=\"preserve\">http://www.google.com</text>\n"
                      "    </link>\n"
                      "    <softbreak />\n"
                      "    <text sourcepos=\"4:1-4:6\" xml:space=\"preserve\">there </text>\n"
                      "    <code sourcepos=\"4:8-4:9\" xml:space=\"preserve\">hi</code>\n"
                      "    <text sourcepos=\"4:11-4:14\" xml:space=\"preserve\"> -- </text>\n"
                      "    <link sourcepos=\"4:15-4:41\" destination=\"www.google.com\" title=\"ok\">\n"
                      "      <text sourcepos=\"4:16-4:19\" xml:space=\"preserve\">okay</text>\n"
                      "    </link>\n"
                      "    <text sourcepos=\"4:42-4:42\" xml:space=\"preserve\">.</text>\n"
                      "  </paragraph>\n"
                      "  <block_quote sourcepos=\"6:1-10:20\">\n"
                      "    <list sourcepos=\"6:3-10:20\" type=\"ordered\" start=\"1\" delim=\"period\" tight=\"false\">\n"
                      "      <item sourcepos=\"6:3-8:1\">\n"
                      "        <paragraph sourcepos=\"6:6-7:10\">\n"
                      "          <text sourcepos=\"6:6-6:10\" xml:space=\"preserve\">Okay.</text>\n"
                      "          <softbreak />\n"
                      "          <text sourcepos=\"7:6-7:10\" xml:space=\"preserve\">Sure.</text>\n"
                      "        </paragraph>\n"
                      "      </item>\n"
                      "      <item sourcepos=\"9:3-10:20\">\n"
                      "        <paragraph sourcepos=\"9:6-10:20\">\n"
                      "          <text sourcepos=\"9:6-9:15\" xml:space=\"preserve\">Yes, okay.</text>\n"
                      "          <softbreak />\n"
                      "          <image sourcepos=\"10:6-10:20\" destination=\"hi\" title=\"yes\">\n"
                      "            <text sourcepos=\"10:8-10:9\" xml:space=\"preserve\">ok</text>\n"
                      "          </image>\n"
                      "        </paragraph>\n"
                      "      </item>\n"
                      "    </list>\n"
                      "  </block_quote>\n"
                      "</document>\n",
         "sourcepos are as expected");
  free(xml);
  cmark_node_free(doc);
}

static void source_pos_inlines(test_batch_runner *runner) {
  {
    static const char markdown[] =
      "*first*\n"
      "second\n";

    cmark_node *doc = cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);
    char *xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT | CMARK_OPT_SOURCEPOS);
    STR_EQ(runner, xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                        "<document sourcepos=\"1:1-2:6\" xmlns=\"http://commonmark.org/xml/1.0\">\n"
                        "  <paragraph sourcepos=\"1:1-2:6\">\n"
                        "    <emph sourcepos=\"1:1-1:7\">\n"
                        "      <text sourcepos=\"1:2-1:6\" xml:space=\"preserve\">first</text>\n"
                        "    </emph>\n"
                        "    <softbreak />\n"
                        "    <text sourcepos=\"2:1-2:6\" xml:space=\"preserve\">second</text>\n"
                        "  </paragraph>\n"
                        "</document>\n",
                        "sourcepos are as expected");
    free(xml);
    cmark_node_free(doc);
  }
  {
    static const char markdown[] =
      "*first\n"
      "second*\n";

    cmark_node *doc = cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);
    char *xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT | CMARK_OPT_SOURCEPOS);
    STR_EQ(runner, xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                        "<document sourcepos=\"1:1-2:7\" xmlns=\"http://commonmark.org/xml/1.0\">\n"
                        "  <paragraph sourcepos=\"1:1-2:7\">\n"
                        "    <emph sourcepos=\"1:1-2:7\">\n"
                        "      <text sourcepos=\"1:2-1:6\" xml:space=\"preserve\">first</text>\n"
                        "      <softbreak />\n"
                        "      <text sourcepos=\"2:1-2:6\" xml:space=\"preserve\">second</text>\n"
                        "    </emph>\n"
                        "  </paragraph>\n"
                        "</document>\n",
                        "sourcepos are as expected");
    free(xml);
    cmark_node_free(doc);
  }
}

static void ref_source_pos(test_batch_runner *runner) {
  static const char markdown[] =
    "Let's try [reference] links.\n"
    "\n"
    "[reference]: https://github.com (GitHub)\n";

  cmark_node *doc = cmark_parse_document(markdown, sizeof(markdown) - 1, CMARK_OPT_DEFAULT);
  char *xml = cmark_render_xml(doc, CMARK_OPT_DEFAULT | CMARK_OPT_SOURCEPOS);
  STR_EQ(runner, xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                      "<document sourcepos=\"1:1-3:40\" xmlns=\"http://commonmark.org/xml/1.0\">\n"
                      "  <paragraph sourcepos=\"1:1-1:28\">\n"
                      "    <text sourcepos=\"1:1-1:10\" xml:space=\"preserve\">Let's try </text>\n"
                      "    <link sourcepos=\"1:11-1:21\" destination=\"https://github.com\" title=\"GitHub\">\n"
                      "      <text sourcepos=\"1:12-1:20\" xml:space=\"preserve\">reference</text>\n"
                      "    </link>\n"
                      "    <text sourcepos=\"1:22-1:28\" xml:space=\"preserve\"> links.</text>\n"
                      "  </paragraph>\n"
                      "</document>\n",
         "sourcepos are as expected");
  free(xml);
  cmark_node_free(doc);
}

int main() {
  int retval;
  test_batch_runner *runner = test_batch_runner_new();

  cmark_enable_safety_checks(true);
  version(runner);
  constructor(runner);
  accessors(runner);
  node_check(runner);
  iterator(runner);
  iterator_delete(runner);
  create_tree(runner);
  custom_nodes(runner);
  hierarchy(runner);
  parser(runner);
  render_html(runner);
  render_xml(runner);
  render_man(runner);
  render_latex(runner);
  render_commonmark(runner);
  render_plaintext(runner);
  utf8(runner);
  line_endings(runner);
  numeric_entities(runner);
  test_cplusplus(runner);
  test_safe(runner);
  test_feed_across_line_ending(runner);
  test_feed_ast(runner);
  test_feed_convergence(runner);
  test_pathological_regressions(runner);
  source_pos(runner);
  source_pos_inlines(runner);
  ref_source_pos(runner);

  test_print_summary(runner);
  retval = test_ok(runner) ? 0 : 1;
  free(runner);

  return retval;
}

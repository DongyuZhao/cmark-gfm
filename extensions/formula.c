#include "formula.h"

#include <chunk.h>
#include <parser.h>
#include <render.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

cmark_node_type CMARK_NODE_INLINE_FORMULA;
cmark_node_type CMARK_NODE_FORMULA_BLOCK;

static cmark_node *match_formula(cmark_syntax_extension *self,
                                 cmark_parser *parser, cmark_node *parent,
                                 unsigned char character,
                                 cmark_inline_parser *inline_parser) {
  if (character != '$') {
    return NULL;
  }

  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  int start = cmark_inline_parser_get_offset(inline_parser);
  const unsigned char *data = chunk->data + start;
  int remaining = (int)(chunk->len - (bufsize_t)start);

  if (remaining < 2) {
    return NULL;
  }

  int delimiter_length = (remaining > 1 && data[1] == '$') ? 2 : 1;

  if (delimiter_length == 1 && remaining > 1 && isspace((unsigned char)data[1])) {
    return NULL;
  }

  int i = delimiter_length;

  while (i < remaining) {
    if (data[i] == '$') {
      int backslashes = 0;
      int scan_back = i - 1;
      while (scan_back >= 0 && data[scan_back] == '\\') {
        backslashes++;
        scan_back--;
      }

      if (backslashes % 2 != 0) {
        i++;
        continue;
      }

      int closing_length = 1;
      while (i + closing_length < remaining && data[i + closing_length] == '$') {
        closing_length++;
      }

      if (closing_length == delimiter_length) {
        if (delimiter_length == 1 && i > 0 && isspace((unsigned char)data[i - 1])) {
          i += closing_length;
          continue;
        }

        bufsize_t content_length = (bufsize_t)(i - delimiter_length);
        unsigned char *content_start = (unsigned char *)(data + delimiter_length);
        cmark_node_type node_type = delimiter_length == 1
                                        ? CMARK_NODE_INLINE_FORMULA
                                        : CMARK_NODE_FORMULA_BLOCK;

        cmark_node *node = cmark_node_new_with_mem(node_type, parser->mem);
        cmark_node_set_syntax_extension(node, self);

        char *tmp = (char *)parser->mem->calloc(1, content_length + 1);
        memcpy(tmp, content_start, content_length);
        cmark_chunk_set_cstr(parser->mem, &node->as.literal, tmp);
        parser->mem->free(tmp);
        node->start_line = node->end_line =
            cmark_inline_parser_get_line(inline_parser);
        node->start_column =
            cmark_inline_parser_get_column(inline_parser) - delimiter_length;

        cmark_inline_parser_set_offset(
            inline_parser, start + i + closing_length);
        return node;
      }

      i += closing_length;
      continue;
    }
    ++i;
  }

  return NULL;
}

static const char *get_type_string(cmark_syntax_extension *extension,
                                   cmark_node *node) {
  if (node->type == CMARK_NODE_INLINE_FORMULA) {
    return "inlineFormula";
  }
  if (node->type == CMARK_NODE_FORMULA_BLOCK) {
    return "formulaBlock";
  }
  return "<unknown>";
}

static int can_contain(cmark_syntax_extension *extension, cmark_node *node,
                       cmark_node_type child_type) {
  return 0;
}

static int contains_inlines(cmark_syntax_extension *extension,
                            cmark_node *node) {
  return 0;
}

static void commonmark_render(cmark_syntax_extension *extension,
                              cmark_renderer *renderer, cmark_node *node,
                              cmark_event_type ev_type, int options) {
  bool entering = ev_type == CMARK_EVENT_ENTER;
  const char *delim =
      node->type == CMARK_NODE_INLINE_FORMULA ? "$" : "$$";

  if (entering) {
    renderer->out(renderer, node, delim, false, LITERAL);
  } else {
    renderer->out(renderer, node, delim, false, LITERAL);
  }
}

static void html_render(cmark_syntax_extension *extension,
                        cmark_html_renderer *renderer, cmark_node *node,
                        cmark_event_type ev_type, int options) {
  bool entering = (ev_type == CMARK_EVENT_ENTER);

  const char *open =
      node->type == CMARK_NODE_INLINE_FORMULA ? "\\(" : "\\[";
  const char *close =
      node->type == CMARK_NODE_INLINE_FORMULA ? "\\)" : "\\]";

  if (entering) {
    cmark_strbuf_puts(renderer->html, open);
    cmark_strbuf_put(renderer->html, node->as.literal.data,
                     node->as.literal.len);
  } else {
    cmark_strbuf_puts(renderer->html, close);
  }
}

static void plaintext_render(cmark_syntax_extension *extension,
                             cmark_renderer *renderer, cmark_node *node,
                             cmark_event_type ev_type, int options) {
  if (ev_type == CMARK_EVENT_ENTER) {
    cmark_renderer_esc(renderer, node->as.literal.data, node->as.literal.len);
  }
}

cmark_syntax_extension *create_formula_extension(void) {
  cmark_syntax_extension *ext = cmark_syntax_extension_new("formula");
  cmark_llist *special_chars = NULL;

  cmark_syntax_extension_set_get_type_string_func(ext, get_type_string);
  cmark_syntax_extension_set_can_contain_func(ext, can_contain);
  cmark_syntax_extension_set_contains_inlines_func(ext, contains_inlines);
  cmark_syntax_extension_set_commonmark_render_func(ext, commonmark_render);
  cmark_syntax_extension_set_plaintext_render_func(ext, plaintext_render);
  cmark_syntax_extension_set_html_render_func(ext, html_render);

  CMARK_NODE_INLINE_FORMULA = cmark_syntax_extension_add_node(1);
  CMARK_NODE_FORMULA_BLOCK = cmark_syntax_extension_add_node(1);

  cmark_syntax_extension_set_match_inline_func(ext, match_formula);

  cmark_mem *mem = cmark_get_default_mem_allocator();
  special_chars = cmark_llist_append(mem, special_chars, (void *)'$');
  cmark_syntax_extension_set_special_inline_chars(ext, special_chars);

  return ext;
}

#include "math.h"

#include <assert.h>
#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <cmark_ctype.h>
#include <html.h>
#include <houdini.h>
#include <node.h>
#include <parser.h>
#include <render.h>

#include "ext_scanners.h"

cmark_node_type CMARK_NODE_MATH_INLINE;
cmark_node_type CMARK_NODE_MATH_BLOCK;

#define MATH_DELIM_DOLLAR_INLINE 1
#define MATH_DELIM_DOLLAR_DISPLAY 2
#define MATH_DELIM_BACKSLASH_INLINE 3
#define MATH_DELIM_BACKSLASH_DISPLAY 4

typedef struct {
  cmark_chunk literal;
  cmark_math_mode mode;
} node_math;

static int is_math_node(cmark_node *node) {
  if (!node)
    return 0;

  return node->type == CMARK_NODE_MATH_INLINE ||
         node->type == CMARK_NODE_MATH_BLOCK;
}

static node_math *get_math(cmark_node *node) {
  if (!is_math_node(node))
    return NULL;

  return (node_math *)node->as.opaque;
}

static int is_standalone_math_node(cmark_node *node) {
  node_math *math = get_math(node);

  if (!math)
    return 0;

  return math->mode == CMARK_MATH_MODE_STANDALONE;
}

const char *cmark_gfm_extensions_get_math_literal(cmark_node *node) {
  node_math *math = get_math(node);
  if (!math)
    return NULL;

  return cmark_chunk_to_cstr(cmark_node_mem(node), &math->literal);
}

int cmark_gfm_extensions_set_math_literal(cmark_node *node,
                                          const char *literal) {
  node_math *math = get_math(node);
  if (!math)
    return 0;

  cmark_chunk_set_cstr(cmark_node_mem(node), &math->literal, literal);
  return 1;
}

cmark_math_mode cmark_gfm_extensions_get_math_mode(cmark_node *node) {
  node_math *math = get_math(node);
  if (!math)
    return CMARK_MATH_MODE_NONE;

  return math->mode;
}

int cmark_gfm_extensions_set_math_mode(cmark_node *node,
                                       cmark_math_mode mode) {
  node_math *math = get_math(node);
  if (!math)
    return 0;

  if (mode != CMARK_MATH_MODE_EMBEDDED &&
      mode != CMARK_MATH_MODE_STANDALONE)
    return 0;

  if (node->type == CMARK_NODE_MATH_BLOCK &&
      mode != CMARK_MATH_MODE_STANDALONE)
    return 0;

  math->mode = mode;
  return 1;
}

static void math_opaque_alloc(cmark_syntax_extension *extension, cmark_mem *mem,
                              cmark_node *node) {
  if (is_math_node(node))
    node->as.opaque = mem->calloc(1, sizeof(node_math));
}

static void math_opaque_free(cmark_syntax_extension *extension, cmark_mem *mem,
                             cmark_node *node) {
  node_math *math = (node_math *)node->as.opaque;
  if (!math)
    return;

  cmark_chunk_free(mem, &math->literal);
  mem->free(math);
}

static int set_math_literal_bytes(cmark_node *node, const unsigned char *data,
                                  bufsize_t len) {
  node_math *math = get_math(node);
  if (!math)
    return 0;

  cmark_chunk_free(cmark_node_mem(node), &math->literal);
  math->literal.data = (unsigned char *)data;
  math->literal.len = len;
  math->literal.alloc = 0;
  cmark_chunk_to_cstr(cmark_node_mem(node), &math->literal);
  return 1;
}

static int set_math_literal_trimmed(cmark_node *node, const unsigned char *data,
                                    bufsize_t len) {
  while (len > 0 && cmark_isspace(data[0])) {
    data++;
    len--;
  }

  while (len > 0 && cmark_isspace(data[len - 1]))
    len--;

  return set_math_literal_bytes(node, data, len);
}

static cmark_node *make_math_node(cmark_syntax_extension *extension,
                                  cmark_parser *parser,
                                  cmark_node_type node_type,
                                  cmark_math_mode mode,
                                  const unsigned char *literal,
                                  bufsize_t literal_len) {
  cmark_node *node =
      cmark_node_new_with_mem_and_ext(node_type, parser->mem, extension);
  if (!node)
    return NULL;

  get_math(node)->mode = mode;
  set_math_literal_bytes(node, literal, literal_len);
  return node;
}

static cmark_node *make_delimiter_text(cmark_parser *parser,
                                       cmark_inline_parser *inline_parser,
                                       bufsize_t len) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  bufsize_t offset = (bufsize_t)cmark_inline_parser_get_offset(inline_parser);
  cmark_node *node = cmark_node_new_with_mem(CMARK_NODE_TEXT, parser->mem);

  if (!node)
    return NULL;

  node->as.literal = cmark_chunk_dup(chunk, offset, len);
  node->start_line = node->end_line =
      cmark_inline_parser_get_line(inline_parser);
  node->start_column = cmark_inline_parser_get_column(inline_parser);
  node->end_column = node->start_column + (int)len - 1;
  cmark_inline_parser_set_offset(inline_parser, (int)(offset + len));
  return node;
}

static cmark_node *match_math_delimiter(cmark_parser *parser,
                                        cmark_inline_parser *inline_parser,
                                        unsigned char delim_char,
                                        bufsize_t len, int can_open,
                                        int can_close) {
  cmark_node *node = make_delimiter_text(parser, inline_parser, len);

  if (!node)
    return NULL;

  cmark_inline_parser_push_delimiter(inline_parser, delim_char, can_open,
                                     can_close, node);
  return node;
}

static bufsize_t scan_backslash_close(const unsigned char *data, bufsize_t len,
                                      bufsize_t offset,
                                      unsigned char close_char) {
  if (offset + 3 <= len && data[offset] == '\\' && data[offset + 1] == '\\' &&
      data[offset + 2] == close_char)
    return 3;

  return 0;
}

static cmark_node *match(cmark_syntax_extension *extension, cmark_parser *parser,
                         cmark_node *parent, unsigned char character,
                         cmark_inline_parser *inline_parser) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  int offset = cmark_inline_parser_get_offset(inline_parser);
  int len = (int)chunk->len;
  bufsize_t opener_len;
  bufsize_t closer_len;

  if (character == '$') {
    if (scan_math_dollar_display_open(chunk->data, len, offset))
      return match_math_delimiter(parser, inline_parser,
                                  MATH_DELIM_DOLLAR_DISPLAY, 2, 1, 1);

    if (scan_math_dollar_inline_open(chunk->data, len, offset))
      return match_math_delimiter(parser, inline_parser,
                                  MATH_DELIM_DOLLAR_INLINE, 1, 1, 1);
  } else if (character == '\\') {
    opener_len = scan_math_backslash_display_open(chunk->data, len, offset);
    if (opener_len)
      return match_math_delimiter(parser, inline_parser,
                                  MATH_DELIM_BACKSLASH_DISPLAY,
                                  opener_len, 1, 0);

    opener_len = scan_math_backslash_inline_open(chunk->data, len, offset);
    if (opener_len)
      return match_math_delimiter(parser, inline_parser,
                                  MATH_DELIM_BACKSLASH_INLINE,
                                  opener_len, 1, 0);

    closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ']');
    if (closer_len)
      return match_math_delimiter(parser, inline_parser,
                                  MATH_DELIM_BACKSLASH_DISPLAY,
                                  closer_len, 0, 1);

    closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ')');
    if (closer_len)
      return match_math_delimiter(parser, inline_parser,
                                  MATH_DELIM_BACKSLASH_INLINE,
                                  closer_len, 0, 1);
  }

  return NULL;
}

static cmark_math_mode mode_for_delim(unsigned char delim_char) {
  return delim_char == MATH_DELIM_DOLLAR_DISPLAY ||
                 delim_char == MATH_DELIM_BACKSLASH_DISPLAY
             ? CMARK_MATH_MODE_STANDALONE
             : CMARK_MATH_MODE_EMBEDDED;
}

static int is_backslash_delim(unsigned char delim_char) {
  return delim_char == MATH_DELIM_BACKSLASH_INLINE ||
         delim_char == MATH_DELIM_BACKSLASH_DISPLAY;
}

static void remove_delimiters(cmark_inline_parser *inline_parser,
                              delimiter *opener, delimiter *closer) {
  delimiter *delim = closer;

  while (delim != NULL && delim != opener) {
    delimiter *previous = delim->previous;
    cmark_inline_parser_remove_delimiter(inline_parser, delim);
    delim = previous;
  }

  cmark_inline_parser_remove_delimiter(inline_parser, opener);
}

static void free_nodes_through(cmark_node *first, cmark_node *last) {
  cmark_node *node = first;

  while (node) {
    cmark_node *next = cmark_node_next(node);
    cmark_node_free(node);
    if (node == last)
      break;
    node = next;
  }
}

static cmark_node *make_backslash_delimited_math(
    cmark_syntax_extension *extension, cmark_parser *parser,
    cmark_math_mode mode, const unsigned char *data, bufsize_t body_start,
    bufsize_t body_end, int slash_count, unsigned char close_char) {
  cmark_strbuf literal;
  bufsize_t i = body_start;
  cmark_node *node;

  cmark_strbuf_init(parser->mem, &literal, 0);

  while (i < body_end) {
    if (slash_count > 1 && data[i] == '\\' && i + 1 < body_end &&
        data[i + 1] == close_char) {
      cmark_strbuf_putc(&literal, close_char);
      i += 2;
      continue;
    }

    cmark_strbuf_putc(&literal, data[i]);
    i++;
  }

  node = make_math_node(extension, parser, CMARK_NODE_MATH_INLINE, mode,
                        literal.ptr, literal.size);
  cmark_strbuf_free(&literal);
  return node;
}

static delimiter *insert_math(cmark_syntax_extension *extension,
                              cmark_parser *parser,
                              cmark_inline_parser *inline_parser,
                              delimiter *opener, delimiter *closer) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  cmark_node *opener_node = opener->inl_text;
  cmark_node *closer_node = closer->inl_text;
  delimiter *res = closer->next;
  cmark_node *math = NULL;
  bufsize_t body_start = opener->position;
  bufsize_t body_end = closer->position - closer->length;
  cmark_math_mode mode = mode_for_delim((unsigned char)opener->delim_char);
  const unsigned char *literal = chunk->data + body_start;
  bufsize_t literal_len = body_end - body_start;

  if (opener->delim_char != closer->delim_char)
    goto done;

  if (opener->length != closer->length &&
      is_backslash_delim((unsigned char)opener->delim_char))
    goto done;

  if (opener->delim_char == MATH_DELIM_DOLLAR_INLINE &&
      literal_len > 0 && literal[0] == '`') {
    if (literal_len < 2 || literal[literal_len - 1] != '`')
      goto done;

    literal++;
    literal_len -= 2;
  }

  if (is_backslash_delim((unsigned char)opener->delim_char)) {
    math = make_backslash_delimited_math(
        extension, parser, mode, chunk->data, body_start, body_end, 2,
        mode == CMARK_MATH_MODE_STANDALONE ? ']' : ')');
  } else {
    math = make_math_node(extension, parser, CMARK_NODE_MATH_INLINE, mode,
                          literal, literal_len);
  }

  if (!math)
    goto done;

  math->start_line = opener_node->start_line;
  math->end_line = closer_node->end_line;
  math->start_column = opener_node->start_column;
  math->end_column = closer_node->end_column;

  if (cmark_node_insert_before(opener_node, math)) {
    free_nodes_through(opener_node, closer_node);
  } else {
    cmark_node_free(math);
  }

done:
  remove_delimiters(inline_parser, opener, closer);
  return res;
}

static int literal_contains_backslash_close(cmark_chunk *literal,
                                            unsigned char close_char) {
  bufsize_t i;

  for (i = 0; i + 1 < literal->len; i++) {
    if (literal->data[i] == '\\' && literal->data[i + 1] == close_char)
      return 1;
  }

  return 0;
}

static void escape_html(cmark_strbuf *html, cmark_chunk *literal) {
  houdini_escape_html0(html, literal->data, literal->len, 0);
}

static void render_html_math(cmark_strbuf *html, cmark_node *node) {
  node_math *math = get_math(node);
  int standalone = is_standalone_math_node(node);
  int use_dollars;

  if (!math)
    return;

  use_dollars = literal_contains_backslash_close(&math->literal,
                                                 standalone ? ']' : ')');

  if (use_dollars) {
    cmark_strbuf_puts(html, standalone ? "$$" : "$");
    escape_html(html, &math->literal);
    cmark_strbuf_puts(html, standalone ? "$$" : "$");
  } else {
    cmark_strbuf_puts(html, standalone ? "\\[" : "\\(");
    escape_html(html, &math->literal);
    cmark_strbuf_puts(html, standalone ? "\\]" : "\\)");
  }
}

static void html_render(cmark_syntax_extension *extension,
                        cmark_html_renderer *renderer, cmark_node *node,
                        cmark_event_type ev_type, int options) {
  if (ev_type != CMARK_EVENT_ENTER)
    return;

  if (node->type == CMARK_NODE_MATH_BLOCK) {
    cmark_html_render_cr(renderer->html);
    cmark_strbuf_puts(renderer->html, "<div class=\"math math-display\">");
    render_html_math(renderer->html, node);
    cmark_strbuf_puts(renderer->html, "</div>\n");
  } else {
    cmark_strbuf_puts(renderer->html, "<span class=\"math ");
    cmark_strbuf_puts(renderer->html,
                      is_standalone_math_node(node) ? "math-display" :
                                                      "math-inline");
    cmark_strbuf_puts(renderer->html, "\">");
    render_html_math(renderer->html, node);
    cmark_strbuf_puts(renderer->html, "</span>");
  }
}

static void commonmark_render(cmark_syntax_extension *extension,
                              cmark_renderer *renderer, cmark_node *node,
                              cmark_event_type ev_type, int options) {
  node_math *math = get_math(node);

  if (!math || ev_type != CMARK_EVENT_ENTER)
    return;

  if (node->type == CMARK_NODE_MATH_BLOCK) {
    renderer->blankline(renderer);
    renderer->out(renderer, node, "$$", false, LITERAL);
    renderer->cr(renderer);
    renderer->out(renderer, node,
                  cmark_chunk_to_cstr(renderer->mem, &math->literal), false,
                  LITERAL);
    renderer->cr(renderer);
    renderer->out(renderer, node, "$$", false, LITERAL);
    renderer->blankline(renderer);
  } else if (is_standalone_math_node(node)) {
    renderer->out(renderer, node, "$$", false, LITERAL);
    renderer->out(renderer, node,
                  cmark_chunk_to_cstr(renderer->mem, &math->literal), false,
                  LITERAL);
    renderer->out(renderer, node, "$$", false, LITERAL);
  } else {
    renderer->out(renderer, node, "$", false, LITERAL);
    renderer->out(renderer, node,
                  cmark_chunk_to_cstr(renderer->mem, &math->literal), false,
                  LITERAL);
    renderer->out(renderer, node, "$", false, LITERAL);
  }
}

static void plaintext_render(cmark_syntax_extension *extension,
                             cmark_renderer *renderer, cmark_node *node,
                             cmark_event_type ev_type, int options) {
  node_math *math = get_math(node);

  if (!math || ev_type != CMARK_EVENT_ENTER)
    return;

  renderer->out(renderer, node, cmark_chunk_to_cstr(renderer->mem,
                                                    &math->literal),
                false, LITERAL);
}

static const char *get_type_string(cmark_syntax_extension *extension,
                                   cmark_node *node) {
  if (node->type == CMARK_NODE_MATH_INLINE)
    return "math_inline";

  if (node->type == CMARK_NODE_MATH_BLOCK)
    return "math_block";

  return "<unknown>";
}

static int can_contain(cmark_syntax_extension *extension, cmark_node *node,
                       cmark_node_type child_type) {
  if (is_math_node(node))
    return 0;

  return 0;
}

static int info_is_math(cmark_chunk *info) {
  return info->len == 4 && memcmp(info->data, "math", 4) == 0;
}

static cmark_node *new_math_block_from_literal(cmark_syntax_extension *extension,
                                               cmark_mem *mem,
                                               cmark_node *oldnode,
                                               const unsigned char *literal,
                                               bufsize_t literal_len) {
  cmark_node *math =
      cmark_node_new_with_mem_and_ext(CMARK_NODE_MATH_BLOCK, mem, extension);
  if (!math)
    return NULL;

  get_math(math)->mode = CMARK_MATH_MODE_STANDALONE;
  math->start_line = oldnode->start_line;
  math->start_column = oldnode->start_column;
  math->end_line = oldnode->end_line;
  math->end_column = oldnode->end_column;
  set_math_literal_trimmed(math, literal, literal_len);
  return math;
}

static void replace_with_math_block(cmark_syntax_extension *extension,
                                    cmark_mem *mem, cmark_node *oldnode,
                                    const unsigned char *literal,
                                    bufsize_t literal_len) {
  cmark_node *math =
      new_math_block_from_literal(extension, mem, oldnode, literal, literal_len);
  if (!math)
    return;

  if (cmark_node_replace(oldnode, math))
    cmark_node_free(oldnode);
  else
    cmark_node_free(math);
}

static void postprocess_node(cmark_syntax_extension *extension,
                             cmark_parser *parser, cmark_node *node) {
  cmark_node *child;
  cmark_node *next;

  if (node->type == CMARK_NODE_CODE_BLOCK && info_is_math(&node->as.code.info)) {
    replace_with_math_block(extension, parser->mem, node,
                            node->as.code.literal.data,
                            node->as.code.literal.len);
    return;
  }

  if (node->type == CMARK_NODE_PARAGRAPH && node->first_child &&
      node->first_child == node->last_child &&
      node->first_child->type == CMARK_NODE_MATH_INLINE &&
      is_standalone_math_node(node->first_child)) {
    node_math *math = get_math(node->first_child);
    if (math) {
      replace_with_math_block(extension, parser->mem, node, math->literal.data,
                              math->literal.len);
      return;
    }
  }

  child = node->first_child;
  while (child) {
    next = child->next;
    postprocess_node(extension, parser, child);
    child = next;
  }
}

static cmark_node *postprocess(cmark_syntax_extension *extension,
                               cmark_parser *parser, cmark_node *root) {
  postprocess_node(extension, parser, root);
  return root;
}

cmark_syntax_extension *create_math_extension(void) {
  cmark_syntax_extension *ext = cmark_syntax_extension_new("math");
  cmark_llist *special_chars = NULL;
  cmark_mem *mem = cmark_get_default_mem_allocator();

  CMARK_NODE_MATH_INLINE = cmark_syntax_extension_add_node(1);
  CMARK_NODE_MATH_BLOCK = cmark_syntax_extension_add_node(0);

  cmark_syntax_extension_set_match_inline_func(ext, match);
  cmark_syntax_extension_set_postprocess_func(ext, postprocess);
  cmark_syntax_extension_set_get_type_string_func(ext, get_type_string);
  cmark_syntax_extension_set_can_contain_func(ext, can_contain);
  cmark_syntax_extension_set_commonmark_render_func(ext, commonmark_render);
  cmark_syntax_extension_set_plaintext_render_func(ext, plaintext_render);
  cmark_syntax_extension_set_html_render_func(ext, html_render);
  cmark_syntax_extension_set_opaque_alloc_func(ext, math_opaque_alloc);
  cmark_syntax_extension_set_opaque_free_func(ext, math_opaque_free);
  cmark_syntax_extension_set_inline_from_delim_func(ext, insert_math);

  special_chars = cmark_llist_append(mem, special_chars, (void *)'$');
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)MATH_DELIM_DOLLAR_INLINE);
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)MATH_DELIM_DOLLAR_DISPLAY);
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)MATH_DELIM_BACKSLASH_INLINE);
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)MATH_DELIM_BACKSLASH_DISPLAY);
  cmark_syntax_extension_set_special_inline_chars(ext, special_chars);
  cmark_syntax_extension_set_emphasis(ext, 1);

  return ext;
}

#include "formula.h"

#include <assert.h>
#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <cmark_ctype.h>
#include <houdini.h>
#include <html.h>
#include <node.h>
#include <parser.h>
#include <render.h>

#include "ext_scanners.h"

cmark_node_type CMARK_NODE_FORMULA_INLINE;
cmark_node_type CMARK_NODE_FORMULA_BLOCK;

#define FORMULA_DELIM_DOLLAR_INLINE 1
#define FORMULA_DELIM_DOLLAR_DISPLAY 2
#define FORMULA_DELIM_LATEX_BACKSLASH_INLINE 3
#define FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY 4
#define FORMULA_DELIM_MS_BACKSLASH_INLINE 5
#define FORMULA_DELIM_MS_BACKSLASH_DISPLAY 6

#define FORMULA_BLOCK_DELIM_NONE 0
#define FORMULA_BLOCK_DELIM_LATEX_BACKSLASH 1
#define FORMULA_BLOCK_DELIM_DOLLAR 2
#define FORMULA_BLOCK_DELIM_MS_BACKSLASH 3

typedef struct {
  cmark_chunk literal;
  cmark_formula_mode mode;
  int block_delim;
  int closed;
} node_formula;

static int is_formula_node(cmark_node *node) {
  if (!node)
    return 0;

  return node->type == CMARK_NODE_FORMULA_INLINE ||
         node->type == CMARK_NODE_FORMULA_BLOCK;
}

static node_formula *get_formula(cmark_node *node) {
  if (!is_formula_node(node))
    return NULL;

  return (node_formula *)node->as.opaque;
}

static int is_standalone_formula_node(cmark_node *node) {
  node_formula *formula = get_formula(node);

  if (!formula)
    return 0;

  return formula->mode == CMARK_FORMULA_MODE_STANDALONE;
}

const char *cmark_gfm_extensions_get_formula_literal(cmark_node *node) {
  node_formula *formula = get_formula(node);
  if (!formula)
    return NULL;

  return cmark_chunk_to_cstr(cmark_node_mem(node), &formula->literal);
}

int cmark_gfm_extensions_set_formula_literal(cmark_node *node,
                                          const char *literal) {
  node_formula *formula = get_formula(node);
  if (!formula)
    return 0;

  cmark_chunk_set_cstr(cmark_node_mem(node), &formula->literal, literal);
  return 1;
}

cmark_formula_mode cmark_gfm_extensions_get_formula_mode(cmark_node *node) {
  node_formula *formula = get_formula(node);
  if (!formula)
    return CMARK_FORMULA_MODE_NONE;

  return formula->mode;
}

int cmark_gfm_extensions_set_formula_mode(cmark_node *node, cmark_formula_mode mode) {
  node_formula *formula = get_formula(node);
  if (!formula)
    return 0;

  if (mode != CMARK_FORMULA_MODE_EMBEDDED && mode != CMARK_FORMULA_MODE_STANDALONE)
    return 0;

  if (node->type == CMARK_NODE_FORMULA_BLOCK &&
      mode != CMARK_FORMULA_MODE_STANDALONE)
    return 0;

  formula->mode = mode;
  return 1;
}

static void formula_opaque_alloc(cmark_syntax_extension *extension, cmark_mem *mem,
                              cmark_node *node) {
  if (is_formula_node(node))
    node->as.opaque = mem->calloc(1, sizeof(node_formula));
}

static void formula_opaque_free(cmark_syntax_extension *extension, cmark_mem *mem,
                             cmark_node *node) {
  node_formula *formula = (node_formula *)node->as.opaque;
  if (!formula)
    return;

  cmark_chunk_free(mem, &formula->literal);
  mem->free(formula);
}

static int set_formula_literal_bytes(cmark_node *node, const unsigned char *data,
                                  bufsize_t len) {
  node_formula *formula = get_formula(node);
  if (!formula)
    return 0;

  cmark_chunk_free(cmark_node_mem(node), &formula->literal);
  formula->literal.data = (unsigned char *)data;
  formula->literal.len = len;
  formula->literal.alloc = 0;
  cmark_chunk_to_cstr(cmark_node_mem(node), &formula->literal);
  return 1;
}

static int set_formula_literal_trimmed(cmark_node *node, const unsigned char *data,
                                    bufsize_t len) {
  while (len > 0 && cmark_isspace(data[0])) {
    data++;
    len--;
  }

  while (len > 0 && cmark_isspace(data[len - 1]))
    len--;

  return set_formula_literal_bytes(node, data, len);
}

static cmark_node *
make_formula_node(cmark_syntax_extension *extension, cmark_parser *parser,
               cmark_node_type node_type, cmark_formula_mode mode,
               const unsigned char *literal, bufsize_t literal_len) {
  cmark_node *node =
      cmark_node_new_with_mem_and_ext(node_type, parser->mem, extension);
  if (!node)
    return NULL;

  get_formula(node)->mode = mode;
  set_formula_literal_bytes(node, literal, literal_len);
  return node;
}

static int is_line_end(const unsigned char *data, bufsize_t len,
                       bufsize_t pos) {
  return pos >= len || data[pos] == '\n' || data[pos] == '\r';
}

static int has_only_spaces_until_line_end(const unsigned char *data,
                                          bufsize_t len, bufsize_t pos) {
  while (pos < len && (data[pos] == ' ' || data[pos] == '\t'))
    pos++;

  return is_line_end(data, len, pos);
}

static int scan_formula_block_open(const unsigned char *data, bufsize_t len,
                                   bufsize_t pos, int latex_formula_delimiters,
                                   int ms_formula_delimiters,
                                   int dollar_formula_delimiters) {
  if (latex_formula_delimiters && pos + 3 <= len && data[pos] == '\\' &&
      data[pos + 1] == '\\' && data[pos + 2] == '[' &&
      has_only_spaces_until_line_end(data, len, pos + 3))
    return FORMULA_BLOCK_DELIM_LATEX_BACKSLASH;

  if (ms_formula_delimiters && pos + 2 <= len && data[pos] == '\\' &&
      data[pos + 1] == '[' &&
      has_only_spaces_until_line_end(data, len, pos + 2))
    return FORMULA_BLOCK_DELIM_MS_BACKSLASH;

  if (dollar_formula_delimiters && pos + 2 <= len && data[pos] == '$' &&
      data[pos + 1] == '$' &&
      has_only_spaces_until_line_end(data, len, pos + 2))
    return FORMULA_BLOCK_DELIM_DOLLAR;

  return FORMULA_BLOCK_DELIM_NONE;
}

static int scan_formula_block_close(const unsigned char *data, bufsize_t len,
                                    bufsize_t pos, int block_delim) {
  if (block_delim == FORMULA_BLOCK_DELIM_LATEX_BACKSLASH) {
    return pos + 3 <= len && data[pos] == '\\' && data[pos + 1] == '\\' &&
           data[pos + 2] == ']' &&
           has_only_spaces_until_line_end(data, len, pos + 3);
  }

  if (block_delim == FORMULA_BLOCK_DELIM_MS_BACKSLASH) {
    return pos + 2 <= len && data[pos] == '\\' && data[pos + 1] == ']' &&
           has_only_spaces_until_line_end(data, len, pos + 2);
  }

  if (block_delim == FORMULA_BLOCK_DELIM_DOLLAR) {
    return pos + 2 <= len && data[pos] == '$' && data[pos + 1] == '$' &&
           has_only_spaces_until_line_end(data, len, pos + 2);
  }

  return 0;
}

static cmark_node *try_opening_formula_block(cmark_syntax_extension *extension,
                                          int indented, cmark_parser *parser,
                                          cmark_node *parent_container,
                                          unsigned char *input, int len) {
  int block_delim;
  cmark_node *node;
  node_formula *formula;
  int first_nonspace = cmark_parser_get_first_nonspace(parser);

  if (indented)
    return NULL;

  block_delim =
      scan_formula_block_open(input, (bufsize_t)len, (bufsize_t)first_nonspace,
                              parser->options & CMARK_OPT_LATEX_FORMULA_DELIMITERS,
                              parser->options & CMARK_OPT_MS_FORMULA_DELIMITERS,
                              parser->options &
                                  CMARK_OPT_DOLLAR_FORMULA_DELIMITERS);
  if (block_delim == FORMULA_BLOCK_DELIM_NONE)
    return NULL;

  node = cmark_parser_add_child(parser, parent_container,
                                CMARK_NODE_FORMULA_BLOCK, first_nonspace + 1);
  if (!node)
    return NULL;

  cmark_node_set_syntax_extension(node, extension);
  node->as.opaque = parser->mem->calloc(1, sizeof(node_formula));

  formula = get_formula(node);
  if (!formula)
    return NULL;

  formula->mode = CMARK_FORMULA_MODE_STANDALONE;
  formula->block_delim = block_delim;
  cmark_parser_advance_offset(parser, (char *)input,
                              len - cmark_parser_get_offset(parser), false);
  return node;
}

static int formula_block_matches(cmark_syntax_extension *extension,
                              cmark_parser *parser, unsigned char *input,
                              int len, cmark_node *container) {
  node_formula *formula = get_formula(container);
  int first_nonspace = cmark_parser_get_first_nonspace(parser);

  if (!formula || formula->closed)
    return 0;

  if (scan_formula_block_close(input, (bufsize_t)len, (bufsize_t)first_nonspace,
                               formula->block_delim)) {
    formula->closed = 1;
    cmark_parser_advance_offset(parser, (char *)input,
                                len - cmark_parser_get_offset(parser), false);
  }

  return 1;
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

static cmark_node *match_formula_delimiter(cmark_parser *parser,
                                        cmark_inline_parser *inline_parser,
                                        unsigned char delim_char, bufsize_t len,
                                        int can_open, int can_close) {
  cmark_node *node = make_delimiter_text(parser, inline_parser, len);

  if (!node)
    return NULL;

  if (can_open || can_close)
    cmark_inline_parser_push_delimiter(inline_parser, delim_char, can_open,
                                       can_close, node);
  return node;
}

static int dollar_inline_can_open(cmark_chunk *chunk, bufsize_t offset) {
  return offset + 1 < chunk->len &&
         !cmark_isspace((char)chunk->data[offset + 1]);
}

static int dollar_inline_can_close(cmark_chunk *chunk, bufsize_t offset) {
  return offset > 0 && !cmark_isspace((char)chunk->data[offset - 1]) &&
         (offset + 1 >= chunk->len ||
          !cmark_isdigit((char)chunk->data[offset + 1]));
}

static bufsize_t scan_backslash_close(const unsigned char *data, bufsize_t len,
                                      bufsize_t offset,
                                      unsigned char close_char,
                                      int slash_count) {
  int i;

  if (offset + slash_count + 1 > len)
    return 0;

  for (i = 0; i < slash_count; i++) {
    if (data[offset + i] != '\\')
      return 0;
  }

  if (data[offset + slash_count] == close_char)
    return (bufsize_t)(slash_count + 1);

  return 0;
}

static int latex_formula_delimiters_enabled(cmark_parser *parser) {
  return parser->options & CMARK_OPT_LATEX_FORMULA_DELIMITERS;
}

static int ms_formula_delimiters_enabled(cmark_parser *parser) {
  return parser->options & CMARK_OPT_MS_FORMULA_DELIMITERS;
}

static int dollar_formula_delimiters_enabled(cmark_parser *parser) {
  return parser->options & CMARK_OPT_DOLLAR_FORMULA_DELIMITERS;
}

static cmark_node *match(cmark_syntax_extension *extension,
                         cmark_parser *parser, cmark_node *parent,
                         unsigned char character,
                         cmark_inline_parser *inline_parser) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  int offset = cmark_inline_parser_get_offset(inline_parser);
  int len = (int)chunk->len;
  bufsize_t opener_len;
  bufsize_t closer_len;

  if (character == '$') {
    if (!dollar_formula_delimiters_enabled(parser))
      return NULL;

    if (scan_formula_dollar_display_open(chunk->data, len, offset))
      return match_formula_delimiter(parser, inline_parser,
                                  FORMULA_DELIM_DOLLAR_DISPLAY, 2, 1, 1);

    if (scan_formula_dollar_inline_open(chunk->data, len, offset))
      return match_formula_delimiter(parser, inline_parser,
                                  FORMULA_DELIM_DOLLAR_INLINE, 1,
                                  dollar_inline_can_open(chunk,
                                                         (bufsize_t)offset),
                                  dollar_inline_can_close(chunk,
                                                          (bufsize_t)offset));
  } else if (character == '\\') {
    if (latex_formula_delimiters_enabled(parser)) {
      opener_len =
          scan_formula_latex_backslash_display_open(chunk->data, len, offset);
      if (opener_len)
        return match_formula_delimiter(parser, inline_parser,
                                    FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY,
                                    opener_len, 1, 0);

      opener_len =
          scan_formula_latex_backslash_inline_open(chunk->data, len, offset);
      if (opener_len)
        return match_formula_delimiter(parser, inline_parser,
                                    FORMULA_DELIM_LATEX_BACKSLASH_INLINE,
                                    opener_len, 1, 0);
    }

    if (ms_formula_delimiters_enabled(parser)) {
      opener_len =
          scan_formula_ms_backslash_display_open(chunk->data, len, offset);
      if (opener_len)
        return match_formula_delimiter(parser, inline_parser,
                                    FORMULA_DELIM_MS_BACKSLASH_DISPLAY, opener_len,
                                    1, 0);

      opener_len = scan_formula_ms_backslash_inline_open(chunk->data, len, offset);
      if (opener_len)
        return match_formula_delimiter(parser, inline_parser,
                                    FORMULA_DELIM_MS_BACKSLASH_INLINE, opener_len,
                                    1, 0);
    }

    if (latex_formula_delimiters_enabled(parser)) {
      closer_len =
          scan_backslash_close(chunk->data, chunk->len, offset, ']', 2);
      if (closer_len)
        return match_formula_delimiter(parser, inline_parser,
                                    FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY,
                                    closer_len, 0, 1);

      closer_len =
          scan_backslash_close(chunk->data, chunk->len, offset, ')', 2);
      if (closer_len)
        return match_formula_delimiter(parser, inline_parser,
                                    FORMULA_DELIM_LATEX_BACKSLASH_INLINE,
                                    closer_len, 0, 1);
    }

    if (ms_formula_delimiters_enabled(parser)) {
      closer_len =
          scan_backslash_close(chunk->data, chunk->len, offset, ']', 1);
      if (closer_len)
        return match_formula_delimiter(parser, inline_parser,
                                    FORMULA_DELIM_MS_BACKSLASH_DISPLAY, closer_len,
                                    0, 1);

      closer_len =
          scan_backslash_close(chunk->data, chunk->len, offset, ')', 1);
      if (closer_len)
        return match_formula_delimiter(parser, inline_parser,
                                    FORMULA_DELIM_MS_BACKSLASH_INLINE, closer_len,
                                    0, 1);
    }
  }

  return NULL;
}

static cmark_formula_mode mode_for_delim(unsigned char delim_char) {
  return delim_char == FORMULA_DELIM_DOLLAR_DISPLAY ||
                 delim_char == FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY ||
                 delim_char == FORMULA_DELIM_MS_BACKSLASH_DISPLAY
             ? CMARK_FORMULA_MODE_STANDALONE
             : CMARK_FORMULA_MODE_EMBEDDED;
}

static int is_backslash_delim(unsigned char delim_char) {
  return delim_char == FORMULA_DELIM_LATEX_BACKSLASH_INLINE ||
         delim_char == FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY ||
         delim_char == FORMULA_DELIM_MS_BACKSLASH_INLINE ||
         delim_char == FORMULA_DELIM_MS_BACKSLASH_DISPLAY;
}

static int slash_count_for_delim(unsigned char delim_char) {
  return delim_char == FORMULA_DELIM_MS_BACKSLASH_INLINE ||
                 delim_char == FORMULA_DELIM_MS_BACKSLASH_DISPLAY
             ? 1
             : 2;
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

static cmark_node *make_backslash_delimited_formula(
    cmark_syntax_extension *extension, cmark_parser *parser,
    cmark_formula_mode mode, const unsigned char *data, bufsize_t body_start,
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

  node = make_formula_node(extension, parser, CMARK_NODE_FORMULA_INLINE, mode,
                        literal.ptr, literal.size);
  cmark_strbuf_free(&literal);
  return node;
}

static delimiter *insert_formula(cmark_syntax_extension *extension,
                              cmark_parser *parser,
                              cmark_inline_parser *inline_parser,
                              delimiter *opener, delimiter *closer) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  cmark_node *opener_node = opener->inl_text;
  cmark_node *closer_node = closer->inl_text;
  delimiter *res = closer->next;
  cmark_node *formula = NULL;
  bufsize_t body_start = opener->position;
  bufsize_t body_end = closer->position - closer->length;
  cmark_formula_mode mode = mode_for_delim((unsigned char)opener->delim_char);
  const unsigned char *literal = chunk->data + body_start;
  bufsize_t literal_len = body_end - body_start;

  if (opener->delim_char != closer->delim_char)
    goto done;

  if (opener->length != closer->length &&
      is_backslash_delim((unsigned char)opener->delim_char))
    goto done;

  if (opener->delim_char == FORMULA_DELIM_DOLLAR_INLINE && literal_len > 0 &&
      literal[0] == '`') {
    if (literal_len < 2 || literal[literal_len - 1] != '`')
      goto done;

    literal++;
    literal_len -= 2;
  }

  if (is_backslash_delim((unsigned char)opener->delim_char)) {
    formula = make_backslash_delimited_formula(
        extension, parser, mode, chunk->data, body_start, body_end,
        slash_count_for_delim((unsigned char)opener->delim_char),
        mode == CMARK_FORMULA_MODE_STANDALONE ? ']' : ')');
  } else {
    formula = make_formula_node(extension, parser, CMARK_NODE_FORMULA_INLINE, mode,
                          literal, literal_len);
  }

  if (!formula)
    goto done;

  formula->start_line = opener_node->start_line;
  formula->end_line = closer_node->end_line;
  formula->start_column = opener_node->start_column;
  formula->end_column = closer_node->end_column;

  if (cmark_node_insert_before(opener_node, formula)) {
    free_nodes_through(opener_node, closer_node);
  } else {
    cmark_node_free(formula);
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

static void render_html_formula(cmark_strbuf *html, cmark_node *node) {
  node_formula *formula = get_formula(node);
  int standalone = is_standalone_formula_node(node);
  int use_dollars;

  if (!formula)
    return;

  use_dollars =
      literal_contains_backslash_close(&formula->literal, standalone ? ']' : ')');

  if (use_dollars) {
    cmark_strbuf_puts(html, standalone ? "$$" : "$");
    escape_html(html, &formula->literal);
    cmark_strbuf_puts(html, standalone ? "$$" : "$");
  } else {
    cmark_strbuf_puts(html, standalone ? "\\[" : "\\(");
    escape_html(html, &formula->literal);
    cmark_strbuf_puts(html, standalone ? "\\]" : "\\)");
  }
}

static void html_render(cmark_syntax_extension *extension,
                        cmark_html_renderer *renderer, cmark_node *node,
                        cmark_event_type ev_type, int options) {
  if (ev_type != CMARK_EVENT_ENTER)
    return;

  if (node->type == CMARK_NODE_FORMULA_BLOCK) {
    cmark_html_render_cr(renderer->html);
    cmark_strbuf_puts(renderer->html, "<div class=\"formula formula-display\">");
    render_html_formula(renderer->html, node);
    cmark_strbuf_puts(renderer->html, "</div>\n");
  } else {
    cmark_strbuf_puts(renderer->html, "<span class=\"formula ");
    cmark_strbuf_puts(renderer->html, is_standalone_formula_node(node)
                                          ? "formula-display"
                                          : "formula-inline");
    cmark_strbuf_puts(renderer->html, "\">");
    render_html_formula(renderer->html, node);
    cmark_strbuf_puts(renderer->html, "</span>");
  }
}

static void commonmark_render(cmark_syntax_extension *extension,
                              cmark_renderer *renderer, cmark_node *node,
                              cmark_event_type ev_type, int options) {
  node_formula *formula = get_formula(node);

  if (!formula || ev_type != CMARK_EVENT_ENTER)
    return;

  if (node->type == CMARK_NODE_FORMULA_BLOCK) {
    renderer->blankline(renderer);
    renderer->out(renderer, node, "$$", false, LITERAL);
    renderer->cr(renderer);
    renderer->out(renderer, node,
                  cmark_chunk_to_cstr(renderer->mem, &formula->literal), false,
                  LITERAL);
    renderer->cr(renderer);
    renderer->out(renderer, node, "$$", false, LITERAL);
    renderer->blankline(renderer);
  } else if (is_standalone_formula_node(node)) {
    renderer->out(renderer, node, "$$", false, LITERAL);
    renderer->out(renderer, node,
                  cmark_chunk_to_cstr(renderer->mem, &formula->literal), false,
                  LITERAL);
    renderer->out(renderer, node, "$$", false, LITERAL);
  } else {
    renderer->out(renderer, node, "$", false, LITERAL);
    renderer->out(renderer, node,
                  cmark_chunk_to_cstr(renderer->mem, &formula->literal), false,
                  LITERAL);
    renderer->out(renderer, node, "$", false, LITERAL);
  }
}

static void plaintext_render(cmark_syntax_extension *extension,
                             cmark_renderer *renderer, cmark_node *node,
                             cmark_event_type ev_type, int options) {
  node_formula *formula = get_formula(node);

  if (!formula || ev_type != CMARK_EVENT_ENTER)
    return;

  renderer->out(renderer, node,
                cmark_chunk_to_cstr(renderer->mem, &formula->literal), false,
                LITERAL);
}

static const char *get_type_string(cmark_syntax_extension *extension,
                                   cmark_node *node) {
  if (node->type == CMARK_NODE_FORMULA_INLINE)
    return "formula_inline";

  if (node->type == CMARK_NODE_FORMULA_BLOCK)
    return "formula_block";

  return "<unknown>";
}

static int can_contain(cmark_syntax_extension *extension, cmark_node *node,
                       cmark_node_type child_type) {
  if (is_formula_node(node))
    return 0;

  return 0;
}

static int accepts_lines(cmark_syntax_extension *extension, cmark_node *node) {
  return node && node->type == CMARK_NODE_FORMULA_BLOCK;
}

static int info_is_formula(cmark_chunk *info) {
  return info->len == 7 && memcmp(info->data, "formula", 7) == 0;
}

static cmark_node *
new_formula_block_from_literal(cmark_syntax_extension *extension, cmark_mem *mem,
                            cmark_node *oldnode, const unsigned char *literal,
                            bufsize_t literal_len) {
  cmark_node *formula =
      cmark_node_new_with_mem_and_ext(CMARK_NODE_FORMULA_BLOCK, mem, extension);
  if (!formula)
    return NULL;

  get_formula(formula)->mode = CMARK_FORMULA_MODE_STANDALONE;
  formula->start_line = oldnode->start_line;
  formula->start_column = oldnode->start_column;
  formula->end_line = oldnode->end_line;
  formula->end_column = oldnode->end_column;
  set_formula_literal_trimmed(formula, literal, literal_len);
  return formula;
}

static void replace_with_formula_block(cmark_syntax_extension *extension,
                                    cmark_mem *mem, cmark_node *oldnode,
                                    const unsigned char *literal,
                                    bufsize_t literal_len) {
  cmark_node *formula = new_formula_block_from_literal(extension, mem, oldnode,
                                                 literal, literal_len);
  if (!formula)
    return;

  if (cmark_node_replace(oldnode, formula))
    cmark_node_free(oldnode);
  else
    cmark_node_free(formula);
}

static void postprocess_node(cmark_syntax_extension *extension,
                             cmark_parser *parser, cmark_node *node) {
  cmark_node *child;
  cmark_node *next;

  if (node->type == CMARK_NODE_FORMULA_BLOCK) {
    node_formula *formula = get_formula(node);
    if (formula && !formula->literal.data) {
      set_formula_literal_trimmed(node, node->content.ptr, node->content.size);
      cmark_strbuf_clear(&node->content);
    }
    return;
  }

  if (node->type == CMARK_NODE_CODE_BLOCK &&
      info_is_formula(&node->as.code.info)) {
    replace_with_formula_block(extension, parser->mem, node,
                            node->as.code.literal.data,
                            node->as.code.literal.len);
    return;
  }

  if (node->type == CMARK_NODE_PARAGRAPH && node->first_child &&
      node->first_child == node->last_child &&
      node->first_child->type == CMARK_NODE_FORMULA_INLINE &&
      is_standalone_formula_node(node->first_child)) {
    node_formula *formula = get_formula(node->first_child);
    if (formula) {
      replace_with_formula_block(extension, parser->mem, node, formula->literal.data,
                              formula->literal.len);
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

cmark_syntax_extension *create_formula_extension(void) {
  cmark_syntax_extension *ext = cmark_syntax_extension_new("formula");
  cmark_llist *special_chars = NULL;
  cmark_mem *mem = cmark_get_default_mem_allocator();

  CMARK_NODE_FORMULA_INLINE = cmark_syntax_extension_add_node(1);
  CMARK_NODE_FORMULA_BLOCK = cmark_syntax_extension_add_node(0);

  cmark_syntax_extension_set_match_inline_func(ext, match);
  cmark_syntax_extension_set_match_block_func(ext, formula_block_matches);
  cmark_syntax_extension_set_open_block_func(ext, try_opening_formula_block);
  cmark_syntax_extension_set_postprocess_func(ext, postprocess);
  cmark_syntax_extension_set_get_type_string_func(ext, get_type_string);
  cmark_syntax_extension_set_can_contain_func(ext, can_contain);
  cmark_syntax_extension_set_accepts_lines_func(ext, accepts_lines);
  cmark_syntax_extension_set_commonmark_render_func(ext, commonmark_render);
  cmark_syntax_extension_set_plaintext_render_func(ext, plaintext_render);
  cmark_syntax_extension_set_html_render_func(ext, html_render);
  cmark_syntax_extension_set_opaque_alloc_func(ext, formula_opaque_alloc);
  cmark_syntax_extension_set_opaque_free_func(ext, formula_opaque_free);
  cmark_syntax_extension_set_inline_from_delim_func(ext, insert_formula);

  special_chars = cmark_llist_append(mem, special_chars, (void *)'$');
  special_chars = cmark_llist_append(mem, special_chars, (void *)'\\');
  special_chars =
      cmark_llist_append(mem, special_chars, (void *)FORMULA_DELIM_DOLLAR_INLINE);
  special_chars =
      cmark_llist_append(mem, special_chars, (void *)FORMULA_DELIM_DOLLAR_DISPLAY);
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)FORMULA_DELIM_LATEX_BACKSLASH_INLINE);
  special_chars = cmark_llist_append(
      mem, special_chars, (void *)FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY);
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)FORMULA_DELIM_MS_BACKSLASH_INLINE);
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)FORMULA_DELIM_MS_BACKSLASH_DISPLAY);
  cmark_syntax_extension_set_special_inline_chars(ext, special_chars);
  cmark_syntax_extension_set_emphasis(ext, 1);

  return ext;
}

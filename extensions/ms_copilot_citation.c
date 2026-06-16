#include "ms_copilot_citation.h"

#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <cmark-gfm.h>
#include <html.h>
#include <houdini.h>
#include <node.h>
#include <parser.h>
#include <render.h>

cmark_node_type CMARK_NODE_MS_COPILOT_CITATION;

typedef struct {
  cmark_chunk ref;
  cmark_chunk xml_attr;
} node_ms_copilot_citation;

#define MS_COPILOT_CITATION_OPEN_STR "\xE3\x80\x90"
#define MS_COPILOT_CITATION_CLOSE_STR "\xE3\x80\x91"

static const unsigned char MS_COPILOT_CITATION_OPEN[] =
    MS_COPILOT_CITATION_OPEN_STR;
static const unsigned char MS_COPILOT_CITATION_CLOSE[] =
    MS_COPILOT_CITATION_CLOSE_STR;
#define MS_COPILOT_CITATION_DELIM_LEN 3
#define MS_COPILOT_CITATION_DELIM 7

static int is_ms_copilot_citation_node(cmark_node *node) {
  return node && node->type == CMARK_NODE_MS_COPILOT_CITATION;
}

static node_ms_copilot_citation *get_ms_copilot_citation(cmark_node *node) {
  if (!is_ms_copilot_citation_node(node))
    return NULL;

  return (node_ms_copilot_citation *)node->as.opaque;
}

static void
clear_ms_copilot_citation_xml_attr(cmark_node *node,
                                   node_ms_copilot_citation *citation) {
  cmark_chunk_free(cmark_node_mem(node), &citation->xml_attr);
}

const char *cmark_gfm_extensions_get_ms_copilot_citation_ref(cmark_node *node) {
  node_ms_copilot_citation *citation = get_ms_copilot_citation(node);
  if (!citation)
    return NULL;

  return cmark_chunk_to_cstr(cmark_node_mem(node), &citation->ref);
}

int cmark_gfm_extensions_set_ms_copilot_citation_ref(cmark_node *node,
                                                     const char *ref) {
  node_ms_copilot_citation *citation = get_ms_copilot_citation(node);
  if (!citation)
    return 0;

  cmark_chunk_set_cstr(cmark_node_mem(node), &citation->ref, ref);
  clear_ms_copilot_citation_xml_attr(node, citation);
  return 1;
}

static void ms_copilot_citation_opaque_alloc(cmark_syntax_extension *extension,
                                             cmark_mem *mem,
                                             cmark_node *node) {
  if (is_ms_copilot_citation_node(node))
    node->as.opaque = mem->calloc(1, sizeof(node_ms_copilot_citation));
}

static void ms_copilot_citation_opaque_free(cmark_syntax_extension *extension,
                                            cmark_mem *mem,
                                            cmark_node *node) {
  node_ms_copilot_citation *citation =
      (node_ms_copilot_citation *)node->as.opaque;
  if (!citation)
    return;

  cmark_chunk_free(mem, &citation->ref);
  cmark_chunk_free(mem, &citation->xml_attr);
  mem->free(citation);
}

static int
set_ms_copilot_citation_ref_bytes(cmark_node *node,
                                  const unsigned char *data, bufsize_t len) {
  node_ms_copilot_citation *citation = get_ms_copilot_citation(node);
  if (!citation)
    return 0;

  cmark_chunk_free(cmark_node_mem(node), &citation->ref);
  citation->ref.data = (unsigned char *)data;
  citation->ref.len = len;
  citation->ref.alloc = 0;
  cmark_chunk_to_cstr(cmark_node_mem(node), &citation->ref);
  clear_ms_copilot_citation_xml_attr(node, citation);
  return 1;
}

static int starts_with(const unsigned char *data, bufsize_t len, bufsize_t pos,
                       const unsigned char *delim) {
  return pos + MS_COPILOT_CITATION_DELIM_LEN <= len &&
         memcmp(data + pos, delim, MS_COPILOT_CITATION_DELIM_LEN) == 0;
}

static cmark_node *
make_ms_copilot_citation_node(cmark_syntax_extension *extension,
                              cmark_parser *parser,
                              const unsigned char *ref,
                              bufsize_t ref_len,
                              int start_line, int start_column,
                              int end_line, int end_column) {
  cmark_node *node =
      cmark_node_new_with_mem_and_ext(CMARK_NODE_MS_COPILOT_CITATION,
                                      parser->mem, extension);
  if (!node)
    return NULL;

  set_ms_copilot_citation_ref_bytes(node, ref, ref_len);
  node->start_line = start_line;
  node->end_line = end_line;
  node->start_column = start_column;
  node->end_column = end_column;
  return node;
}

static cmark_node *make_delimiter_text(cmark_parser *parser,
                                       cmark_inline_parser *inline_parser,
                                       bufsize_t offset, bufsize_t len) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
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

static cmark_node *match_citation_delimiter(cmark_parser *parser,
                                            cmark_inline_parser *inline_parser,
                                            bufsize_t offset, bufsize_t len,
                                            int can_open, int can_close) {
  cmark_node *node =
      make_delimiter_text(parser, inline_parser, offset, len);

  if (!node)
    return NULL;

  cmark_inline_parser_push_delimiter(inline_parser, MS_COPILOT_CITATION_DELIM,
                                     can_open, can_close, node);
  return node;
}

static cmark_node *match(cmark_syntax_extension *extension, cmark_parser *parser,
                         cmark_node *parent, unsigned char character,
                         cmark_inline_parser *inline_parser) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  bufsize_t offset = (bufsize_t)cmark_inline_parser_get_offset(inline_parser);
  unsigned char *data = chunk->data;
  bufsize_t len = chunk->len;
  bufsize_t opener_start = offset;

  if (!(parser->options & CMARK_OPT_MS_COPILOT_CITATION))
    return NULL;

  if (character == '!') {
    opener_start++;
    if (opener_start >= len)
      return NULL;

    if (!starts_with(data, len, opener_start, MS_COPILOT_CITATION_OPEN))
      return NULL;

    return match_citation_delimiter(
        parser, inline_parser, offset,
        1 + MS_COPILOT_CITATION_DELIM_LEN, 1, 0);
  }

  if (character != MS_COPILOT_CITATION_OPEN[0])
    return NULL;

  if (starts_with(data, len, opener_start, MS_COPILOT_CITATION_OPEN))
    return match_citation_delimiter(parser, inline_parser, offset,
                                    MS_COPILOT_CITATION_DELIM_LEN, 1, 0);

  if (starts_with(data, len, opener_start, MS_COPILOT_CITATION_CLOSE))
    return match_citation_delimiter(parser, inline_parser, offset,
                                    MS_COPILOT_CITATION_DELIM_LEN, 0, 1);

  return NULL;
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

static delimiter *insert_citation(cmark_syntax_extension *extension,
                                  cmark_parser *parser,
                                  cmark_inline_parser *inline_parser,
                                  delimiter *opener, delimiter *closer) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  cmark_node *opener_node = opener->inl_text;
  cmark_node *closer_node = closer->inl_text;
  delimiter *res = closer->next;
  bufsize_t body_start = opener->position;
  bufsize_t body_end = closer->position - closer->length;
  cmark_node *citation;

  citation = make_ms_copilot_citation_node(
      extension, parser, chunk->data + body_start, body_end - body_start,
      opener_node->start_line, opener_node->start_column,
      closer_node->end_line, closer_node->end_column);

  if (citation && cmark_node_insert_before(opener_node, citation)) {
    free_nodes_through(opener_node, closer_node);
  } else if (citation) {
    cmark_node_free(citation);
  }

  remove_delimiters(inline_parser, opener, closer);
  return res;
}

static const char *get_type_string(cmark_syntax_extension *extension,
                                   cmark_node *node) {
  return node->type == CMARK_NODE_MS_COPILOT_CITATION
             ? "ms_copilot_citation"
             : "<unknown>";
}

static int can_contain(cmark_syntax_extension *extension, cmark_node *node,
                       cmark_node_type child_type) {
  return 0;
}

static void render_ref(cmark_strbuf *html, cmark_chunk *ref) {
  houdini_escape_html0(html, ref->data, ref->len, 0);
}

static void html_render(cmark_syntax_extension *extension,
                        cmark_html_renderer *renderer, cmark_node *node,
                        cmark_event_type ev_type, int options) {
  node_ms_copilot_citation *citation = get_ms_copilot_citation(node);

  if (!citation || ev_type != CMARK_EVENT_ENTER)
    return;

  cmark_strbuf_puts(renderer->html,
                    "<span class=\"ms-copilot-citation\" data-ref=\"");
  render_ref(renderer->html, &citation->ref);
  cmark_strbuf_puts(renderer->html, "\"></span>");
}

static void commonmark_render(cmark_syntax_extension *extension,
                              cmark_renderer *renderer, cmark_node *node,
                              cmark_event_type ev_type, int options) {
  node_ms_copilot_citation *citation = get_ms_copilot_citation(node);

  if (!citation || ev_type != CMARK_EVENT_ENTER)
    return;

  renderer->out(renderer, node, MS_COPILOT_CITATION_OPEN_STR, false, LITERAL);
  renderer->out(renderer, node,
                cmark_chunk_to_cstr(renderer->mem, &citation->ref), false,
                LITERAL);
  renderer->out(renderer, node, MS_COPILOT_CITATION_CLOSE_STR, false, LITERAL);
}

static void plaintext_render(cmark_syntax_extension *extension,
                             cmark_renderer *renderer, cmark_node *node,
                             cmark_event_type ev_type, int options) {
  node_ms_copilot_citation *citation = get_ms_copilot_citation(node);

  if (!citation || ev_type != CMARK_EVENT_ENTER)
    return;

  renderer->out(renderer, node,
                cmark_chunk_to_cstr(renderer->mem, &citation->ref), false,
                LITERAL);
}

static const char *xml_attr(cmark_syntax_extension *extension,
                            cmark_node *node) {
  node_ms_copilot_citation *citation = get_ms_copilot_citation(node);
  cmark_strbuf attr;

  if (!citation)
    return NULL;

  if (citation->xml_attr.data)
    return (const char *)citation->xml_attr.data;

  cmark_strbuf_init(cmark_node_mem(node), &attr, 0);
  cmark_strbuf_puts(&attr, " ref=\"");
  houdini_escape_html0(&attr, citation->ref.data, citation->ref.len, 0);
  cmark_strbuf_putc(&attr, '"');
  citation->xml_attr = cmark_chunk_buf_detach(&attr);
  return (const char *)citation->xml_attr.data;
}

cmark_syntax_extension *create_ms_copilot_citation_extension(void) {
  cmark_syntax_extension *ext =
      cmark_syntax_extension_new("ms_copilot_citation");
  cmark_llist *special_chars = NULL;
  cmark_mem *mem = cmark_get_default_mem_allocator();

  CMARK_NODE_MS_COPILOT_CITATION = cmark_syntax_extension_add_node(1);

  cmark_syntax_extension_set_match_inline_func(ext, match);
  cmark_syntax_extension_set_inline_from_delim_func(ext, insert_citation);
  cmark_syntax_extension_set_get_type_string_func(ext, get_type_string);
  cmark_syntax_extension_set_can_contain_func(ext, can_contain);
  cmark_syntax_extension_set_commonmark_render_func(ext, commonmark_render);
  cmark_syntax_extension_set_plaintext_render_func(ext, plaintext_render);
  cmark_syntax_extension_set_html_render_func(ext, html_render);
  cmark_syntax_extension_set_xml_attr_func(ext, xml_attr);
  cmark_syntax_extension_set_opaque_alloc_func(
      ext, ms_copilot_citation_opaque_alloc);
  cmark_syntax_extension_set_opaque_free_func(
      ext, ms_copilot_citation_opaque_free);

  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)(size_t)
                                         MS_COPILOT_CITATION_OPEN[0]);
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)MS_COPILOT_CITATION_DELIM);
  cmark_syntax_extension_set_special_inline_chars(ext, special_chars);
  cmark_syntax_extension_set_emphasis(ext, 1);

  return ext;
}

#include "ms_copilot_citation.h"

#include <string.h>

#include <buffer.h>
#include <chunk.h>
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

static bufsize_t skip_matched_code_span(const unsigned char *data,
                                        bufsize_t len, bufsize_t pos) {
  bufsize_t opener_len = 0;
  bufsize_t i;

  while (pos + opener_len < len && data[pos + opener_len] == '`')
    opener_len++;

  i = pos + opener_len;
  while (i < len) {
    bufsize_t closer_len = 0;

    if (data[i] != '`') {
      i++;
      continue;
    }

    while (i + closer_len < len && data[i + closer_len] == '`')
      closer_len++;

    if (closer_len == opener_len)
      return i + closer_len;

    i += closer_len;
  }

  return pos + 1;
}

static bufsize_t find_ms_copilot_citation_close(const unsigned char *data,
                                                bufsize_t len,
                                                bufsize_t start) {
  bufsize_t i = start;
  int depth = 0;

  while (i < len) {
    if (data[i] == '`') {
      i = skip_matched_code_span(data, len, i);
      continue;
    }

    if (starts_with(data, len, i, MS_COPILOT_CITATION_OPEN)) {
      depth++;
      i += MS_COPILOT_CITATION_DELIM_LEN;
      continue;
    }

    if (starts_with(data, len, i, MS_COPILOT_CITATION_CLOSE)) {
      if (depth == 0)
        return i;

      depth--;
      i += MS_COPILOT_CITATION_DELIM_LEN;
      continue;
    }

    i++;
  }

  return len;
}

static cmark_node *
make_ms_copilot_citation_node(cmark_syntax_extension *extension,
                              cmark_parser *parser,
                              cmark_inline_parser *inline_parser,
                              bufsize_t body_start, bufsize_t body_end,
                              int consumed) {
  int start_column = cmark_inline_parser_get_column(inline_parser);
  cmark_node *node =
      cmark_node_new_with_mem_and_ext(CMARK_NODE_MS_COPILOT_CITATION,
                                      parser->mem, extension);
  if (!node)
    return NULL;

  set_ms_copilot_citation_ref_bytes(
      node, cmark_inline_parser_get_chunk(inline_parser)->data + body_start,
      body_end - body_start);
  node->start_line = node->end_line =
      cmark_inline_parser_get_line(inline_parser);
  node->start_column = start_column;
  node->end_column = start_column + consumed - 1;
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
  bufsize_t body_start;
  bufsize_t body_end;
  int consumed;
  cmark_node *node;

  if (character == '!') {
    opener_start++;
    if (opener_start >= len)
      return NULL;
  } else if (character != MS_COPILOT_CITATION_OPEN[0]) {
    return NULL;
  }

  if (!starts_with(data, len, opener_start, MS_COPILOT_CITATION_OPEN))
    return NULL;

  body_start = opener_start + MS_COPILOT_CITATION_DELIM_LEN;
  body_end = find_ms_copilot_citation_close(data, len, body_start);
  if (body_end == len)
    return NULL;

  consumed = (int)(body_end + MS_COPILOT_CITATION_DELIM_LEN - offset);
  node = make_ms_copilot_citation_node(extension, parser, inline_parser,
                                       body_start, body_end, consumed);
  if (!node)
    return NULL;

  cmark_inline_parser_set_offset(inline_parser,
                                 (int)(body_end +
                                       MS_COPILOT_CITATION_DELIM_LEN));
  return node;
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
  special_chars = cmark_llist_append(mem, special_chars, (void *)'!');
  cmark_syntax_extension_set_special_inline_chars(ext, special_chars);

  return ext;
}

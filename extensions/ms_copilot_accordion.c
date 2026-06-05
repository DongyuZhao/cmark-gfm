#include "ms_copilot_accordion.h"

#include <string.h>

#include <chunk.h>
#include <cmark-gfm.h>
#include <html.h>
#include <node.h>
#include <parser.h>
#include <render.h>

cmark_node_type CMARK_NODE_MS_COPILOT_ACCORDION;
cmark_node_type CMARK_NODE_MS_COPILOT_ACCORDION_HEADER;
cmark_node_type CMARK_NODE_MS_COPILOT_ACCORDION_CONTENT;

#define MS_COPILOT_ACCORDION_MAX_DEPTH 64

typedef struct {
  bufsize_t start;
  bufsize_t open_end;
  bufsize_t summary_open_end;
  bufsize_t summary_close_start;
  bufsize_t summary_close_end;
  bufsize_t close_start;
  bufsize_t close_end;
} details_match;

static void postprocess_node(cmark_syntax_extension *extension,
                             cmark_parser *parser, cmark_node *node,
                             int depth);

static int ascii_is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static unsigned char ascii_lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int tag_name_matches(const unsigned char *data, bufsize_t len,
                            bufsize_t pos, const char *name,
                            bufsize_t name_len) {
  bufsize_t i;

  if (pos + name_len > len)
    return 0;

  for (i = 0; i < name_len; i++) {
    if (ascii_lower(data[pos + i]) != (unsigned char)name[i])
      return 0;
  }

  return 1;
}

static int scan_tag_end(const unsigned char *data, bufsize_t len,
                        bufsize_t pos, bufsize_t *tag_end) {
  while (pos < len) {
    if (data[pos] == '>') {
      *tag_end = pos + 1;
      return 1;
    }
    pos++;
  }

  return 0;
}

static int match_tag(const unsigned char *data, bufsize_t len, bufsize_t pos,
                     const char *name, int closing, bufsize_t *tag_end) {
  bufsize_t name_len = (bufsize_t)strlen(name);
  bufsize_t name_start;
  unsigned char after_name;

  if (pos + 2 >= len || data[pos] != '<')
    return 0;

  name_start = pos + 1;
  if (closing) {
    if (data[name_start] != '/')
      return 0;
    name_start++;
  } else if (data[name_start] == '/') {
    return 0;
  }

  if (!tag_name_matches(data, len, name_start, name, name_len))
    return 0;

  if (name_start + name_len >= len)
    return 0;

  after_name = data[name_start + name_len];
  if (!(after_name == '>' || after_name == '/' || ascii_is_space(after_name)))
    return 0;

  return scan_tag_end(data, len, name_start + name_len, tag_end);
}

static bufsize_t skip_spaces(const unsigned char *data, bufsize_t len,
                             bufsize_t pos) {
  while (pos < len && ascii_is_space(data[pos]))
    pos++;

  return pos;
}

static int find_summary_close(const unsigned char *data, bufsize_t len,
                              bufsize_t start, bufsize_t stop,
                              bufsize_t *close_start,
                              bufsize_t *close_end) {
  bufsize_t pos;
  bufsize_t tag_end;

  for (pos = start; pos < stop; pos++) {
    if (match_tag(data, len, pos, "summary", 1, &tag_end) &&
        tag_end <= stop) {
      *close_start = pos;
      *close_end = tag_end;
      return 1;
    }
  }

  return 0;
}

static int find_details_close(const unsigned char *data, bufsize_t len,
                              bufsize_t start, bufsize_t *close_start,
                              bufsize_t *close_end) {
  bufsize_t pos = start;
  bufsize_t tag_end;
  int depth = 1;

  while (pos < len) {
    if (match_tag(data, len, pos, "details", 0, &tag_end)) {
      depth++;
      pos = tag_end;
      continue;
    }

    if (match_tag(data, len, pos, "details", 1, &tag_end)) {
      depth--;
      if (depth == 0) {
        *close_start = pos;
        *close_end = tag_end;
        return 1;
      }
      pos = tag_end;
      continue;
    }

    pos++;
  }

  return 0;
}

static int parse_details_match(const unsigned char *data, bufsize_t len,
                               bufsize_t start, details_match *details) {
  bufsize_t tag_end;
  bufsize_t summary_start;

  if (!match_tag(data, len, start, "details", 0, &tag_end))
    return 0;

  details->start = start;
  details->open_end = tag_end;

  summary_start = skip_spaces(data, len, tag_end);
  if (!match_tag(data, len, summary_start, "summary", 0,
                 &details->summary_open_end))
    return 0;

  if (!find_summary_close(data, len, details->summary_open_end,
                          len,
                          &details->summary_close_start,
                          &details->summary_close_end))
    return 0;

  if (!find_details_close(data, len, details->summary_close_end,
                          &details->close_start, &details->close_end)) {
    details->close_start = len;
    details->close_end = len;
  }

  return 1;
}

static int parse_partial_details_match(const unsigned char *data,
                                       bufsize_t len, bufsize_t start,
                                       details_match *details) {
  bufsize_t tag_end;
  bufsize_t summary_start;

  if (!match_tag(data, len, start, "details", 0, &tag_end))
    return 0;

  details->start = start;
  details->open_end = tag_end;
  details->close_start = len;
  details->close_end = len;

  summary_start = skip_spaces(data, len, tag_end);
  if (!match_tag(data, len, summary_start, "summary", 0,
                 &details->summary_open_end))
    return 0;

  if (!find_summary_close(data, len, details->summary_open_end, len,
                          &details->summary_close_start,
                          &details->summary_close_end))
    return 0;

  return 1;
}

static int find_next_details(const unsigned char *data, bufsize_t len,
                             bufsize_t start, details_match *details) {
  bufsize_t pos = start;
  bufsize_t tag_end;

  while (pos < len) {
    while (pos < len && data[pos] != '<')
      pos++;

    if (pos >= len)
      break;

    if (!match_tag(data, len, pos, "details", 0, &tag_end)) {
      pos++;
      continue;
    }

    if (parse_details_match(data, len, pos, details))
      return 1;

    pos = tag_end;
  }

  return 0;
}

static void attach_parser_extensions(cmark_parser *source,
                                     cmark_parser *destination) {
  cmark_llist *it;

  for (it = source->syntax_extensions; it; it = it->next) {
    cmark_parser_attach_syntax_extension(
        destination, (cmark_syntax_extension *)it->data);
  }
}

static void append_markdown_blocks(cmark_syntax_extension *extension,
                                   cmark_parser *parser, cmark_node *parent,
                                   const unsigned char *data, bufsize_t len) {
  cmark_parser *child_parser;
  cmark_node *document;
  cmark_node *child;

  if (len == 0)
    return;

  child_parser = cmark_parser_new_with_mem(parser->options, parser->mem);
  attach_parser_extensions(parser, child_parser);
  cmark_parser_feed(child_parser, (const char *)data, len);
  document = cmark_parser_finish(child_parser);

  while (document && document->first_child) {
    child = document->first_child;
    cmark_node_append_child(parent, child);
  }

  if (document)
    cmark_node_free(document);
  cmark_parser_free(child_parser);
}

static void insert_markdown_blocks_after(cmark_syntax_extension *extension,
                                         cmark_parser *parser,
                                         cmark_node **anchor,
                                         const unsigned char *data,
                                         bufsize_t len) {
  cmark_parser *child_parser;
  cmark_node *document;
  cmark_node *child;

  if (len == 0 || anchor == NULL || *anchor == NULL)
    return;

  child_parser = cmark_parser_new_with_mem(parser->options, parser->mem);
  attach_parser_extensions(parser, child_parser);
  cmark_parser_feed(child_parser, (const char *)data, len);
  document = cmark_parser_finish(child_parser);

  while (document && document->first_child) {
    child = document->first_child;
    if (cmark_node_insert_after(*anchor, child)) {
      *anchor = child;
    } else {
      cmark_node_free(child);
    }
  }

  if (document)
    cmark_node_free(document);
  cmark_parser_free(child_parser);
}

static void own_inline_literals(cmark_node *node) {
  cmark_node *child;

  // Force borrowed inline chunks to own their bytes before the source block is
  // freed.
  switch (node->type) {
  case CMARK_NODE_TEXT:
  case CMARK_NODE_CODE:
  case CMARK_NODE_HTML_INLINE:
  case CMARK_NODE_FOOTNOTE_REFERENCE:
    cmark_node_get_literal(node);
    break;
  case CMARK_NODE_LINK:
  case CMARK_NODE_IMAGE:
    cmark_node_get_url(node);
    cmark_node_get_title(node);
    break;
  default:
    break;
  }

  child = node->first_child;
  while (child) {
    own_inline_literals(child);
    child = child->next;
  }
}

static void append_header_inlines(cmark_syntax_extension *extension,
                                  cmark_parser *parser, cmark_node *header,
                                  const unsigned char *data, bufsize_t len) {
  cmark_parser *child_parser;
  cmark_node *document;
  cmark_node *block;
  cmark_node *child;
  int first_block = 1;

  child_parser = cmark_parser_new_with_mem(parser->options, parser->mem);
  attach_parser_extensions(parser, child_parser);
  cmark_parser_feed(child_parser, (const char *)data, len);
  document = cmark_parser_finish(child_parser);

  while (document && document->first_child) {
    block = document->first_child;
    if (block->type == CMARK_NODE_PARAGRAPH ||
        block->type == CMARK_NODE_HEADING) {
      if (!first_block) {
        cmark_node_append_child(header,
                                cmark_node_new_with_mem(CMARK_NODE_SOFTBREAK,
                                                        parser->mem));
      }
      while (block->first_child) {
        child = block->first_child;
        cmark_node_append_child(header, child);
        own_inline_literals(child);
      }
      first_block = 0;
    }
    cmark_node_free(block);
  }

  if (document)
    cmark_node_free(document);
  cmark_parser_free(child_parser);
}

static int append_accordion_sequence(cmark_syntax_extension *extension,
                                     cmark_parser *parser, cmark_node *parent,
                                     const unsigned char *data,
                                     bufsize_t len, int depth);

static cmark_node *new_extension_node(cmark_syntax_extension *extension,
                                      cmark_parser *parser,
                                      cmark_node_type type) {
  return cmark_node_new_with_mem_and_ext(type, parser->mem, extension);
}

static cmark_node *accordion_from_match(cmark_syntax_extension *extension,
                                        cmark_parser *parser,
                                        const unsigned char *data,
                                        details_match *details, int depth) {
  cmark_node *accordion =
      new_extension_node(extension, parser, CMARK_NODE_MS_COPILOT_ACCORDION);
  cmark_node *header = new_extension_node(
      extension, parser, CMARK_NODE_MS_COPILOT_ACCORDION_HEADER);
  cmark_node *content = new_extension_node(
      extension, parser, CMARK_NODE_MS_COPILOT_ACCORDION_CONTENT);

  if (!accordion || !header || !content) {
    if (accordion)
      cmark_node_free(accordion);
    if (header)
      cmark_node_free(header);
    if (content)
      cmark_node_free(content);
    return NULL;
  }

  append_header_inlines(extension, parser, header,
                        data + details->summary_open_end,
                        details->summary_close_start -
                            details->summary_open_end);
  if (append_accordion_sequence(extension, parser, content,
                                data + details->summary_close_end,
                                details->close_start -
                                    details->summary_close_end,
                                depth + 1) < 0) {
    cmark_node_free(accordion);
    cmark_node_free(header);
    cmark_node_free(content);
    return NULL;
  }

  cmark_node_append_child(accordion, header);
  cmark_node_append_child(accordion, content);
  return accordion;
}

static cmark_node *accordion_from_partial_match(cmark_syntax_extension *extension,
                                                cmark_parser *parser,
                                                const unsigned char *data,
                                                details_match *details,
                                                cmark_node **content_out,
                                                int depth) {
  cmark_node *accordion =
      new_extension_node(extension, parser, CMARK_NODE_MS_COPILOT_ACCORDION);
  cmark_node *header = new_extension_node(
      extension, parser, CMARK_NODE_MS_COPILOT_ACCORDION_HEADER);
  cmark_node *content = new_extension_node(
      extension, parser, CMARK_NODE_MS_COPILOT_ACCORDION_CONTENT);

  if (!accordion || !header || !content) {
    if (accordion)
      cmark_node_free(accordion);
    if (header)
      cmark_node_free(header);
    if (content)
      cmark_node_free(content);
    return NULL;
  }

  append_header_inlines(extension, parser, header,
                        data + details->summary_open_end,
                        details->summary_close_start -
                            details->summary_open_end);
  if (append_accordion_sequence(extension, parser, content,
                                data + details->summary_close_end,
                                details->close_start -
                                    details->summary_close_end,
                                depth + 1) < 0) {
    cmark_node_free(accordion);
    cmark_node_free(header);
    cmark_node_free(content);
    return NULL;
  }

  cmark_node_append_child(accordion, header);
  cmark_node_append_child(accordion, content);
  *content_out = content;
  return accordion;
}

static int append_accordion_sequence(cmark_syntax_extension *extension,
                                     cmark_parser *parser, cmark_node *parent,
                                     const unsigned char *data,
                                     bufsize_t len, int depth) {
  details_match details;
  bufsize_t pos = 0;
  int count = 0;
  cmark_node *accordion;

  while (find_next_details(data, len, pos, &details)) {
    if (depth >= MS_COPILOT_ACCORDION_MAX_DEPTH)
      return -1;

    append_markdown_blocks(extension, parser, parent, data + pos,
                           details.start - pos);
    accordion = accordion_from_match(extension, parser, data, &details, depth);
    if (!accordion)
      return -1;

    cmark_node_append_child(parent, accordion);
    count++;
    pos = details.close_end;
  }

  append_markdown_blocks(extension, parser, parent, data + pos, len - pos);
  return count;
}

static int replace_html_block(cmark_syntax_extension *extension,
                              cmark_parser *parser, cmark_node *node,
                              int depth) {
  cmark_node *fragment;
  cmark_node *child;
  int count;

  if (node->type != CMARK_NODE_HTML_BLOCK || node->as.literal.len == 0)
    return 0;

  if (!find_next_details(node->as.literal.data, node->as.literal.len, 0,
                         &(details_match){0}))
    return 0;

  fragment = cmark_node_new_with_mem(CMARK_NODE_DOCUMENT, parser->mem);
  count = append_accordion_sequence(extension, parser, fragment,
                                    node->as.literal.data,
                                    node->as.literal.len, depth);

  if (count <= 0) {
    cmark_node_free(fragment);
    return 0;
  }

  while (fragment->first_child) {
    child = fragment->first_child;
    cmark_node_insert_before(node, child);
  }

  cmark_node_free(fragment);
  cmark_node_free(node);
  return 1;
}

static int scan_details_depth(const unsigned char *data, bufsize_t len,
                              int *depth, bufsize_t *close_start,
                              bufsize_t *close_end) {
  bufsize_t pos = 0;
  bufsize_t tag_end;

  while (pos < len) {
    while (pos < len && data[pos] != '<')
      pos++;

    if (pos >= len)
      break;

    if (match_tag(data, len, pos, "details", 0, &tag_end)) {
      (*depth)++;
      pos = tag_end;
      continue;
    }

    if (match_tag(data, len, pos, "details", 1, &tag_end)) {
      (*depth)--;
      if (*depth == 0) {
        *close_start = pos;
        *close_end = tag_end;
        return 1;
      }
      pos = tag_end;
      continue;
    }

    pos++;
  }

  return 0;
}

static int has_matching_closing_details_sibling(cmark_node *start) {
  cmark_node *node;
  bufsize_t close_start;
  bufsize_t close_end;
  int depth = 1;

  for (node = start->next; node; node = node->next) {
    if (node->type == CMARK_NODE_HTML_BLOCK &&
        scan_details_depth(node->as.literal.data, node->as.literal.len, &depth,
                           &close_start, &close_end))
      return 1;
  }

  return 0;
}

static int replace_partial_html_block(cmark_syntax_extension *extension,
                                      cmark_parser *parser, cmark_node *node,
                                      cmark_node **next_after, int depth) {
  details_match details;
  cmark_node *accordion;
  cmark_node *content = NULL;
  cmark_node *current;
  cmark_node *next;
  bufsize_t close_start;
  bufsize_t close_end;
  int scan_depth = 1;

  if (node->type != CMARK_NODE_HTML_BLOCK || node->as.literal.len == 0)
    return 0;

  if (depth >= MS_COPILOT_ACCORDION_MAX_DEPTH)
    return 0;

  if (!parse_partial_details_match(node->as.literal.data, node->as.literal.len,
                                   0, &details))
    return 0;

  if (!has_matching_closing_details_sibling(node))
    return 0;

  accordion =
      accordion_from_partial_match(extension, parser, node->as.literal.data,
                                   &details, &content, depth);
  if (!accordion)
    return 0;

  cmark_node_insert_before(node, accordion);

  current = node;
  while (current) {
    next = current->next;

    if (current != node && current->type == CMARK_NODE_HTML_BLOCK &&
        scan_details_depth(current->as.literal.data, current->as.literal.len,
                           &scan_depth, &close_start, &close_end)) {
      append_markdown_blocks(extension, parser, content,
                             current->as.literal.data, close_start);
      insert_markdown_blocks_after(extension, parser, &accordion,
                                   current->as.literal.data + close_end,
                                   current->as.literal.len - close_end);
      cmark_node_free(current);
      postprocess_node(extension, parser, content, depth + 1);
      *next_after = accordion->next;
      return 1;
    }

    if (current != node) {
      cmark_node_append_child(content, current);
    } else {
      cmark_node_free(current);
    }

    current = next;
  }

  *next_after = NULL;
  return 1;
}

static void postprocess_node(cmark_syntax_extension *extension,
                             cmark_parser *parser, cmark_node *node,
                             int depth) {
  cmark_node *child;
  cmark_node *next;
  cmark_node *next_after;
  int child_depth;

  if (replace_html_block(extension, parser, node, depth))
    return;

  child = node->first_child;
  while (child) {
    next = child->next;
    if (replace_partial_html_block(extension, parser, child, &next_after,
                                   depth)) {
      child = next_after;
      continue;
    }
    child_depth = child->type == CMARK_NODE_MS_COPILOT_ACCORDION
                      ? depth + 1
                      : depth;
    postprocess_node(extension, parser, child, child_depth);
    child = next;
  }
}

static cmark_node *postprocess(cmark_syntax_extension *extension,
                               cmark_parser *parser, cmark_node *root) {
  postprocess_node(extension, parser, root, 0);
  return root;
}

static const char *get_type_string(cmark_syntax_extension *extension,
                                   cmark_node *node) {
  if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION)
    return "ms-copilot-accordion";
  if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION_HEADER)
    return "header";
  if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION_CONTENT)
    return "content";

  return "<unknown>";
}

static int can_contain(cmark_syntax_extension *extension, cmark_node *node,
                       cmark_node_type child_type) {
  if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION) {
    return child_type == CMARK_NODE_MS_COPILOT_ACCORDION_HEADER ||
           child_type == CMARK_NODE_MS_COPILOT_ACCORDION_CONTENT;
  }

  if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION_HEADER)
    return CMARK_NODE_TYPE_INLINE_P(child_type);

  if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION_CONTENT)
    return CMARK_NODE_TYPE_BLOCK_P(child_type);

  return 0;
}

static void html_render(cmark_syntax_extension *extension,
                        cmark_html_renderer *renderer, cmark_node *node,
                        cmark_event_type ev_type, int options) {
  int entering = ev_type == CMARK_EVENT_ENTER;

  if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION) {
    if (entering) {
      cmark_html_render_cr(renderer->html);
      cmark_strbuf_puts(renderer->html, "<details>");
      cmark_html_render_cr(renderer->html);
    } else {
      cmark_html_render_cr(renderer->html);
      cmark_strbuf_puts(renderer->html, "</details>");
      cmark_html_render_cr(renderer->html);
    }
  } else if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION_HEADER) {
    cmark_strbuf_puts(renderer->html, entering ? "<summary>" : "</summary>");
  }
}

static void commonmark_render(cmark_syntax_extension *extension,
                              cmark_renderer *renderer, cmark_node *node,
                              cmark_event_type ev_type, int options) {
  int entering = ev_type == CMARK_EVENT_ENTER;

  if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION) {
    if (entering) {
      renderer->out(renderer, node, "<details>", false, LITERAL);
      renderer->cr(renderer);
    } else {
      renderer->out(renderer, node, "</details>", false, LITERAL);
      renderer->cr(renderer);
    }
  } else if (node->type == CMARK_NODE_MS_COPILOT_ACCORDION_HEADER) {
    renderer->out(renderer, node, entering ? "<summary>" : "</summary>", false,
                  LITERAL);
    if (!entering)
      renderer->cr(renderer);
  }
}

cmark_syntax_extension *create_ms_copilot_accordion_extension(void) {
  cmark_syntax_extension *ext =
      cmark_syntax_extension_new("ms_copilot_accordion");

  CMARK_NODE_MS_COPILOT_ACCORDION = cmark_syntax_extension_add_node(0);
  CMARK_NODE_MS_COPILOT_ACCORDION_HEADER =
      cmark_syntax_extension_add_node(0);
  CMARK_NODE_MS_COPILOT_ACCORDION_CONTENT =
      cmark_syntax_extension_add_node(0);

  cmark_syntax_extension_set_postprocess_func(ext, postprocess);
  cmark_syntax_extension_set_get_type_string_func(ext, get_type_string);
  cmark_syntax_extension_set_can_contain_func(ext, can_contain);
  cmark_syntax_extension_set_html_render_func(ext, html_render);
  cmark_syntax_extension_set_commonmark_render_func(ext, commonmark_render);

  return ext;
}

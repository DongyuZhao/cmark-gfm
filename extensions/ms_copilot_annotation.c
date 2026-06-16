#include "ms_copilot_annotation.h"

#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <cmark-gfm.h>
#include <html.h>
#include <houdini.h>
#include <node.h>
#include <parser.h>
#include <render.h>

cmark_node_type CMARK_NODE_MS_COPILOT_ANNOTATION;

typedef struct {
  const char *tag;
  const char *scenario;
} annotation_tag;

typedef struct {
  int closing;
  const annotation_tag *tag;
} parsed_annotation_tag;

typedef struct {
  cmark_chunk scenario;
  cmark_chunk xml_attr;
} node_ms_copilot_annotation;

typedef struct annotation_stack_item {
  size_t index;
  struct annotation_stack_item *previous;
} annotation_stack_item;

typedef struct {
  cmark_node *node;
  cmark_node *closer;
  size_t closer_index;
} annotation_scan_entry;

static const annotation_tag ANNOTATION_TAGS[] = {
    {"Person", "person"},       {"People", "person"},
    {"Email", "email"},         {"cite", "citation"},
    {"File", "file"},           {"Event", "event"},
    {"External", "external"},   {"TeamsMessage", "teamsmessage"},
};

#define ANNOTATION_TAG_COUNT                                                 \
  (sizeof(ANNOTATION_TAGS) / sizeof(ANNOTATION_TAGS[0]))

typedef struct {
  cmark_mem *mem;
  annotation_scan_entry *entries;
  size_t len;
  size_t cap;
  annotation_stack_item *stacks[ANNOTATION_TAG_COUNT];
} annotation_scan;

static void postprocess_node(cmark_syntax_extension *extension,
                             cmark_parser *parser, cmark_node *node);

static int is_ms_copilot_annotation_node(cmark_node *node) {
  return node && node->type == CMARK_NODE_MS_COPILOT_ANNOTATION;
}

static node_ms_copilot_annotation *
get_ms_copilot_annotation(cmark_node *node) {
  if (!is_ms_copilot_annotation_node(node))
    return NULL;

  return (node_ms_copilot_annotation *)node->as.opaque;
}

static void
clear_ms_copilot_annotation_xml_attr(cmark_node *node,
                                     node_ms_copilot_annotation *annotation) {
  cmark_chunk_free(cmark_node_mem(node), &annotation->xml_attr);
}

const char *
cmark_gfm_extensions_get_ms_copilot_annotation_scenario(cmark_node *node) {
  node_ms_copilot_annotation *annotation = get_ms_copilot_annotation(node);
  if (!annotation)
    return NULL;

  return cmark_chunk_to_cstr(cmark_node_mem(node), &annotation->scenario);
}

int cmark_gfm_extensions_set_ms_copilot_annotation_scenario(cmark_node *node,
                                                            const char *scenario) {
  node_ms_copilot_annotation *annotation = get_ms_copilot_annotation(node);
  if (!annotation || !scenario)
    return 0;

  cmark_chunk_set_cstr(cmark_node_mem(node), &annotation->scenario, scenario);
  clear_ms_copilot_annotation_xml_attr(node, annotation);
  return 1;
}

static void
ms_copilot_annotation_opaque_alloc(cmark_syntax_extension *extension,
                                   cmark_mem *mem, cmark_node *node) {
  if (is_ms_copilot_annotation_node(node))
    node->as.opaque = mem->calloc(1, sizeof(node_ms_copilot_annotation));
}

static void
ms_copilot_annotation_opaque_free(cmark_syntax_extension *extension,
                                  cmark_mem *mem, cmark_node *node) {
  node_ms_copilot_annotation *annotation =
      (node_ms_copilot_annotation *)node->as.opaque;
  if (!annotation)
    return;

  cmark_chunk_free(mem, &annotation->scenario);
  cmark_chunk_free(mem, &annotation->xml_attr);
  mem->free(annotation);
}

static int ascii_is_alpha(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int ascii_is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static unsigned char ascii_lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bufsize_t skip_spaces(const unsigned char *data, bufsize_t len,
                             bufsize_t pos) {
  while (pos < len && ascii_is_space(data[pos]))
    pos++;

  return pos;
}

static int ascii_case_equal(const unsigned char *data, bufsize_t len,
                            const char *expected) {
  bufsize_t i;
  bufsize_t expected_len = (bufsize_t)strlen(expected);

  if (len != expected_len)
    return 0;

  for (i = 0; i < len; i++) {
    if (ascii_lower(data[i]) != ascii_lower((unsigned char)expected[i]))
      return 0;
  }

  return 1;
}

static const annotation_tag *find_annotation_tag(const unsigned char *data,
                                                 bufsize_t len) {
  size_t i;

  for (i = 0; i < sizeof(ANNOTATION_TAGS) / sizeof(ANNOTATION_TAGS[0]); i++) {
    if (ascii_case_equal(data, len, ANNOTATION_TAGS[i].tag))
      return &ANNOTATION_TAGS[i];
  }

  return NULL;
}

static int parse_annotation_html_tag(cmark_node *node,
                                     parsed_annotation_tag *parsed) {
  const unsigned char *data;
  bufsize_t len;
  bufsize_t pos;
  bufsize_t name_start;
  bufsize_t name_len;
  bufsize_t end;

  if (!node || node->type != CMARK_NODE_HTML_INLINE ||
      node->as.literal.len < 3)
    return 0;

  data = node->as.literal.data;
  len = node->as.literal.len;
  if (data[0] != '<' || data[len - 1] != '>')
    return 0;

  pos = 1;
  parsed->closing = 0;
  if (data[pos] == '/') {
    parsed->closing = 1;
    pos++;
  }

  name_start = pos;
  while (pos < len && ascii_is_alpha(data[pos]))
    pos++;

  name_len = pos - name_start;
  if (name_len == 0)
    return 0;

  parsed->tag = find_annotation_tag(data + name_start, name_len);
  if (!parsed->tag)
    return 0;

  if (parsed->closing) {
    pos = skip_spaces(data, len, pos);
    return pos + 1 == len && data[pos] == '>';
  }

  if (pos >= len ||
      !(data[pos] == '>' || data[pos] == '/' || ascii_is_space(data[pos])))
    return 0;

  end = len - 1;
  while (end > pos && ascii_is_space(data[end - 1]))
    end--;

  return data[end - 1] != '/';
}

static size_t annotation_tag_index(const annotation_tag *tag) {
  return (size_t)(tag - ANNOTATION_TAGS);
}

static int annotation_scan_append(annotation_scan *scan, cmark_node *node) {
  annotation_scan_entry *entries;
  size_t new_cap;

  if (scan->len == scan->cap) {
    new_cap = scan->cap == 0 ? 16 : scan->cap * 2;
    entries = (annotation_scan_entry *)scan->mem->realloc(
        scan->entries, new_cap * sizeof(annotation_scan_entry));
    if (!entries)
      return 0;
    scan->entries = entries;
    scan->cap = new_cap;
  }

  scan->entries[scan->len].node = node;
  scan->entries[scan->len].closer = NULL;
  scan->entries[scan->len].closer_index = 0;
  scan->len++;
  return 1;
}

static int annotation_scan_push(annotation_scan *scan, const annotation_tag *tag,
                                size_t index) {
  size_t tag_index = annotation_tag_index(tag);
  annotation_stack_item *item =
      (annotation_stack_item *)scan->mem->calloc(1, sizeof(*item));
  if (!item)
    return 0;

  item->index = index;
  item->previous = scan->stacks[tag_index];
  scan->stacks[tag_index] = item;
  return 1;
}

static int annotation_scan_pop(annotation_scan *scan, const annotation_tag *tag,
                               size_t *index) {
  size_t tag_index = annotation_tag_index(tag);
  annotation_stack_item *item = scan->stacks[tag_index];

  if (!item)
    return 0;

  *index = item->index;
  scan->stacks[tag_index] = item->previous;
  scan->mem->free(item);
  return 1;
}

static void annotation_scan_free(annotation_scan *scan) {
  size_t i;

  if (scan->entries)
    scan->mem->free(scan->entries);

  for (i = 0; i < ANNOTATION_TAG_COUNT; i++) {
    while (scan->stacks[i]) {
      annotation_stack_item *item = scan->stacks[i];
      scan->stacks[i] = item->previous;
      scan->mem->free(item);
    }
  }
}

static int build_annotation_scan(cmark_parser *parser, cmark_node *parent,
                                 annotation_scan *scan) {
  cmark_node *child;

  memset(scan, 0, sizeof(*scan));
  scan->mem = parser->mem;

  for (child = parent->first_child; child; child = child->next) {
    parsed_annotation_tag tag;
    size_t index;
    size_t opener_index;

    if (!annotation_scan_append(scan, child))
      return 0;

    index = scan->len - 1;
    if (!parse_annotation_html_tag(child, &tag))
      continue;

    if (tag.closing) {
      if (annotation_scan_pop(scan, tag.tag, &opener_index)) {
        scan->entries[opener_index].closer = child;
        scan->entries[opener_index].closer_index = index;
      }
    } else if (!annotation_scan_push(scan, tag.tag, index)) {
      return 0;
    }
  }

  return 1;
}

static cmark_node *new_annotation_node(cmark_syntax_extension *extension,
                                       cmark_parser *parser,
                                       const char *scenario,
                                       cmark_node *opener,
                                       cmark_node *closer) {
  cmark_node *annotation =
      cmark_node_new_with_mem_and_ext(CMARK_NODE_MS_COPILOT_ANNOTATION,
                                      parser->mem, extension);
  if (!annotation)
    return NULL;

  if (!cmark_gfm_extensions_set_ms_copilot_annotation_scenario(annotation,
                                                               scenario)) {
    cmark_node_free(annotation);
    return NULL;
  }
  annotation->start_line = opener->start_line;
  annotation->start_column = opener->start_column;
  annotation->end_line = closer->end_line;
  annotation->end_column = closer->end_column;
  return annotation;
}

static int wrap_annotation_at(cmark_syntax_extension *extension,
                              cmark_parser *parser, cmark_node *opener,
                              cmark_node *closer) {
  parsed_annotation_tag opening_tag;
  cmark_node *annotation;
  cmark_node *child;
  cmark_node *next;

  if (!parse_annotation_html_tag(opener, &opening_tag) || opening_tag.closing)
    return 0;

  annotation = new_annotation_node(extension, parser, opening_tag.tag->scenario,
                                   opener, closer);
  if (!annotation)
    return 0;

  if (!cmark_node_insert_before(opener, annotation)) {
    cmark_node_free(annotation);
    return 0;
  }

  child = opener->next;
  while (child && child != closer) {
    next = child->next;
    cmark_node_append_child(annotation, child);
    child = next;
  }

  cmark_node_free(opener);
  cmark_node_free(closer);

  if (annotation->first_child) {
    return 1;
  } else {
    cmark_node_free(annotation);
    return 1;
  }
}

static void postprocess_node(cmark_syntax_extension *extension,
                             cmark_parser *parser, cmark_node *node) {
  annotation_scan scan;
  size_t i;

  if (!build_annotation_scan(parser, node, &scan)) {
    annotation_scan_free(&scan);
    return;
  }

  for (i = 0; i < scan.len; i++) {
    cmark_node *child = scan.entries[i].node;

    if (child->parent != node)
      continue;

    if (scan.entries[i].closer &&
        wrap_annotation_at(extension, parser, child, scan.entries[i].closer)) {
      i = scan.entries[i].closer_index;
      continue;
    }

    if (child->type != CMARK_NODE_MS_COPILOT_ANNOTATION)
      postprocess_node(extension, parser, child);
  }

  annotation_scan_free(&scan);
}

static cmark_node *postprocess(cmark_syntax_extension *extension,
                               cmark_parser *parser, cmark_node *root) {
  if (!(parser->options & CMARK_OPT_MS_COPILOT_ANNOTATION))
    return root;

  postprocess_node(extension, parser, root);
  cmark_consolidate_text_nodes(root);
  return root;
}

static const char *get_type_string(cmark_syntax_extension *extension,
                                   cmark_node *node) {
  return node->type == CMARK_NODE_MS_COPILOT_ANNOTATION
             ? "ms-copilot-annotation"
             : "<unknown>";
}

static int can_contain(cmark_syntax_extension *extension, cmark_node *node,
                       cmark_node_type child_type) {
  if (node->type == CMARK_NODE_MS_COPILOT_ANNOTATION)
    return CMARK_NODE_TYPE_INLINE_P(child_type);

  return 0;
}

static int chunk_equals_cstr(cmark_chunk *chunk, const char *str) {
  size_t len = strlen(str);
  return chunk->len == (bufsize_t)len &&
         memcmp(chunk->data, str, len) == 0;
}

static const char *scenario_tag_name(cmark_chunk *scenario) {
  if (chunk_equals_cstr(scenario, "person"))
    return "Person";
  if (chunk_equals_cstr(scenario, "email"))
    return "Email";
  if (chunk_equals_cstr(scenario, "citation"))
    return "cite";
  if (chunk_equals_cstr(scenario, "file"))
    return "File";
  if (chunk_equals_cstr(scenario, "event"))
    return "Event";
  if (chunk_equals_cstr(scenario, "external"))
    return "External";
  if (chunk_equals_cstr(scenario, "teamsmessage"))
    return "TeamsMessage";

  return "Person";
}

static void render_literal(cmark_renderer *renderer, cmark_node *node,
                           const char *literal) {
  renderer->out(renderer, node, literal, false, LITERAL);
}

static void commonmark_render(cmark_syntax_extension *extension,
                              cmark_renderer *renderer, cmark_node *node,
                              cmark_event_type ev_type, int options) {
  node_ms_copilot_annotation *annotation =
      get_ms_copilot_annotation(node);
  const char *tag_name;

  if (!annotation)
    return;

  tag_name = scenario_tag_name(&annotation->scenario);
  if (ev_type == CMARK_EVENT_ENTER) {
    render_literal(renderer, node, "<");
    render_literal(renderer, node, tag_name);
    render_literal(renderer, node, ">");
  } else {
    render_literal(renderer, node, "</");
    render_literal(renderer, node, tag_name);
    render_literal(renderer, node, ">");
  }
}

static void passthrough_render(cmark_syntax_extension *extension,
                               cmark_renderer *renderer, cmark_node *node,
                               cmark_event_type ev_type, int options) {
}

static void render_scenario(cmark_strbuf *html, cmark_chunk *scenario) {
  houdini_escape_html0(html, scenario->data, scenario->len, 0);
}

static void html_render(cmark_syntax_extension *extension,
                        cmark_html_renderer *renderer, cmark_node *node,
                        cmark_event_type ev_type, int options) {
  node_ms_copilot_annotation *annotation =
      get_ms_copilot_annotation(node);

  if (!annotation)
    return;

  if (ev_type == CMARK_EVENT_ENTER) {
    cmark_strbuf_puts(renderer->html, "<span data-entity=\"");
    render_scenario(renderer->html, &annotation->scenario);
    cmark_strbuf_puts(renderer->html, "\">");
  } else {
    cmark_strbuf_puts(renderer->html, "</span>");
  }
}

static const char *xml_attr(cmark_syntax_extension *extension,
                            cmark_node *node) {
  node_ms_copilot_annotation *annotation =
      get_ms_copilot_annotation(node);
  cmark_strbuf attr;

  if (!annotation)
    return NULL;

  if (annotation->xml_attr.data)
    return (const char *)annotation->xml_attr.data;

  cmark_strbuf_init(cmark_node_mem(node), &attr, 0);
  cmark_strbuf_puts(&attr, " scenario=\"");
  houdini_escape_html0(&attr, annotation->scenario.data,
                       annotation->scenario.len, 0);
  cmark_strbuf_putc(&attr, '"');
  annotation->xml_attr = cmark_chunk_buf_detach(&attr);
  return (const char *)annotation->xml_attr.data;
}

cmark_syntax_extension *create_ms_copilot_annotation_extension(void) {
  cmark_syntax_extension *ext =
      cmark_syntax_extension_new("ms_copilot_annotation");

  CMARK_NODE_MS_COPILOT_ANNOTATION = cmark_syntax_extension_add_node(1);

  cmark_syntax_extension_set_postprocess_func(ext, postprocess);
  cmark_syntax_extension_set_get_type_string_func(ext, get_type_string);
  cmark_syntax_extension_set_can_contain_func(ext, can_contain);
  cmark_syntax_extension_set_commonmark_render_func(ext, commonmark_render);
  cmark_syntax_extension_set_plaintext_render_func(ext, passthrough_render);
  cmark_syntax_extension_set_latex_render_func(ext, passthrough_render);
  cmark_syntax_extension_set_html_render_func(ext, html_render);
  cmark_syntax_extension_set_xml_attr_func(ext, xml_attr);
  cmark_syntax_extension_set_opaque_alloc_func(
      ext, ms_copilot_annotation_opaque_alloc);
  cmark_syntax_extension_set_opaque_free_func(
      ext, ms_copilot_annotation_opaque_free);

  return ext;
}

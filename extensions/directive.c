#include "directive.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <cmark-gfm.h>
#include <houdini.h>
#include <html.h>
#include <inlines.h>
#include <node.h>
#include <parser.h>
#include <render.h>
#include <utf8.h>

#include "ext_scanners.h"

#define DIRECTIVE_LABEL_DELIM 8
#define DIRECTIVE_ATTR_DELIM 9

cmark_node_type CMARK_NODE_DIRECTIVE_INLINE;
cmark_node_type CMARK_NODE_DIRECTIVE_BLOCK;
cmark_node_type CMARK_NODE_DIRECTIVE_LABEL;

typedef struct directive_attribute {
  cmark_chunk name;
  cmark_chunk value;
  struct directive_attribute *next;
} directive_attribute;

typedef struct {
  cmark_chunk name;
  directive_attribute *attributes;
  cmark_chunk attributes_json;
  cmark_chunk xml_attr;
  int fence_length;
  int closed;
  int consume_line;
  int has_label;
  int has_attributes;
} node_directive;

typedef struct {
  bufsize_t name_start;
  bufsize_t name_len;
  bufsize_t label_start;
  bufsize_t label_len;
  int has_label;
  int has_attributes;
  directive_attribute *attributes;
  bufsize_t end;
} parsed_directive;

static int is_directive_node(cmark_node *node) {
  return node && (node->type == CMARK_NODE_DIRECTIVE_INLINE ||
                  node->type == CMARK_NODE_DIRECTIVE_BLOCK);
}

static int is_label_node(cmark_node *node) {
  return node && node->type == CMARK_NODE_DIRECTIVE_LABEL;
}

static node_directive *get_directive(cmark_node *node) {
  if (!is_directive_node(node))
    return NULL;

  return (node_directive *)node->as.opaque;
}

static int directive_enabled(cmark_parser *parser) {
  return parser->options & CMARK_OPT_DIRECTIVE;
}

static int ascii_is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int ascii_is_line_space(unsigned char c) {
  return c == ' ' || c == '\t';
}

static unsigned char ascii_lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int is_attr_name_char(unsigned char c);

static int is_line_end(const unsigned char *data, bufsize_t len,
                       bufsize_t pos) {
  return pos >= len || data[pos] == '\n' || data[pos] == '\r';
}

static int has_only_spaces_until_line_end(const unsigned char *data,
                                          bufsize_t len, bufsize_t pos) {
  while (pos < len && ascii_is_line_space(data[pos]))
    pos++;

  return is_line_end(data, len, pos);
}

static int scan_name(unsigned char *data, bufsize_t len, bufsize_t pos,
                     bufsize_t *name_start, bufsize_t *name_len) {
  bufsize_t match_len = scan_directive_name(data, len, pos);
  if (match_len == 0)
    return 0;

  if (data[pos + match_len - 1] == '-' || data[pos + match_len - 1] == '_')
    return 0;

  *name_start = pos;
  *name_len = match_len;
  return 1;
}

static int scan_label(const unsigned char *data, bufsize_t len, bufsize_t pos,
                      bufsize_t *label_start, bufsize_t *label_len,
                      bufsize_t *end) {
  int depth = 1;
  bufsize_t i;

  if (pos >= len || data[pos] != '[')
    return 0;

  i = pos + 1;
  while (i < len) {
    if (data[i] == '\\' && i + 1 < len) {
      i += 2;
      continue;
    }

    if (data[i] == '[') {
      depth++;
      i++;
      continue;
    }

    if (data[i] == ']') {
      depth--;
      if (depth == 0) {
        *label_start = pos + 1;
        *label_len = i - (pos + 1);
        *end = i + 1;
        return 1;
      }
    }

    i++;
  }

  return 0;
}

static int scan_attributes_raw(const unsigned char *data, bufsize_t len,
                               bufsize_t pos, bufsize_t *attr_start,
                               bufsize_t *attr_len, bufsize_t *end) {
  unsigned char quote = 0;
  bufsize_t i;

  if (pos >= len || data[pos] != '{')
    return 0;

  i = pos + 1;
  while (i < len) {
    if (quote) {
      if (data[i] == quote)
        quote = 0;
      i++;
      continue;
    }

    if (data[i] == '"' || data[i] == '\'') {
      quote = data[i];
      i++;
      continue;
    }

    if (data[i] == '}') {
      *attr_start = pos + 1;
      *attr_len = i - (pos + 1);
      *end = i + 1;
      return 1;
    }

    i++;
  }

  return 0;
}

static void clear_xml_attr(cmark_node *node, node_directive *directive) {
  cmark_chunk_free(cmark_node_mem(node), &directive->xml_attr);
}

static void clear_attribute_caches(cmark_node *node,
                                   node_directive *directive) {
  cmark_chunk_free(cmark_node_mem(node), &directive->attributes_json);
  clear_xml_attr(node, directive);
}

static void set_chunk_bytes(cmark_mem *mem, cmark_chunk *chunk,
                            const unsigned char *data, bufsize_t len) {
  cmark_chunk_free(mem, chunk);
  chunk->data = (unsigned char *)data;
  chunk->len = len;
  chunk->alloc = 0;
  cmark_chunk_to_cstr(mem, chunk);
}

static void free_attribute_list(cmark_mem *mem, directive_attribute *attr) {
  while (attr) {
    directive_attribute *next = attr->next;
    cmark_chunk_free(mem, &attr->name);
    cmark_chunk_free(mem, &attr->value);
    mem->free(attr);
    attr = next;
  }
}

static int attribute_name_equals(directive_attribute *attr,
                                 const unsigned char *name,
                                 bufsize_t name_len) {
  return attr->name.len == name_len &&
         memcmp(attr->name.data, name, name_len) == 0;
}

static int attribute_name_is_valid(const unsigned char *name,
                                   bufsize_t name_len) {
  bufsize_t i;

  if (name_len == 0)
    return 0;

  for (i = 0; i < name_len; i++) {
    if (!is_attr_name_char(name[i]))
      return 0;
  }

  return 1;
}

static int set_or_append_attribute(cmark_mem *mem, directive_attribute **head,
                                   directive_attribute **tail,
                                   const unsigned char *name,
                                   bufsize_t name_len,
                                   const unsigned char *value,
                                   bufsize_t value_len) {
  directive_attribute *attr;

  if (!attribute_name_is_valid(name, name_len))
    return 0;

  for (attr = *head; attr; attr = attr->next) {
    if (attribute_name_equals(attr, name, name_len)) {
      set_chunk_bytes(mem, &attr->value, value, value_len);
      return 1;
    }
  }

  attr = (directive_attribute *)mem->calloc(1, sizeof(*attr));
  if (!attr)
    return 0;

  set_chunk_bytes(mem, &attr->name, name, name_len);
  set_chunk_bytes(mem, &attr->value, value, value_len);

  if (*tail) {
    (*tail)->next = attr;
  } else {
    *head = attr;
  }
  *tail = attr;
  return 1;
}

static void remove_attribute(cmark_mem *mem, directive_attribute **head,
                             directive_attribute **tail,
                             const unsigned char *name, bufsize_t name_len) {
  directive_attribute *previous = NULL;
  directive_attribute *attr = *head;

  while (attr) {
    directive_attribute *next = attr->next;
    if (attribute_name_equals(attr, name, name_len)) {
      if (previous)
        previous->next = next;
      else
        *head = next;
      if (*tail == attr)
        *tail = previous;
      attr->next = NULL;
      free_attribute_list(mem, attr);
      return;
    }
    previous = attr;
    attr = next;
  }
}

static void append_json_escaped(cmark_strbuf *buf, const unsigned char *data,
                                bufsize_t len) {
  bufsize_t i;
  char encoded[7];

  for (i = 0; i < len; i++) {
    unsigned char c = data[i];
    switch (c) {
    case '"':
      cmark_strbuf_puts(buf, "\\\"");
      break;
    case '\\':
      cmark_strbuf_puts(buf, "\\\\");
      break;
    case '\b':
      cmark_strbuf_puts(buf, "\\b");
      break;
    case '\f':
      cmark_strbuf_puts(buf, "\\f");
      break;
    case '\n':
      cmark_strbuf_puts(buf, "\\n");
      break;
    case '\r':
      cmark_strbuf_puts(buf, "\\r");
      break;
    case '\t':
      cmark_strbuf_puts(buf, "\\t");
      break;
    default:
      if (c < 0x20) {
        snprintf(encoded, sizeof(encoded), "\\u%04x", c);
        cmark_strbuf_puts(buf, encoded);
      } else {
        cmark_strbuf_putc(buf, c);
      }
    }
  }
}

static void render_one_json_attribute(cmark_strbuf *json,
                                      directive_attribute *attr,
                                      int *first) {
  if (!*first)
    cmark_strbuf_putc(json, ',');
  *first = 0;
  cmark_strbuf_putc(json, '"');
  append_json_escaped(json, attr->name.data, attr->name.len);
  cmark_strbuf_puts(json, "\":\"");
  append_json_escaped(json, attr->value.data, attr->value.len);
  cmark_strbuf_putc(json, '"');
}

static const char *render_attributes_json(cmark_node *node,
                                          node_directive *directive) {
  cmark_strbuf json;
  directive_attribute *attr;
  directive_attribute *id_attr = NULL;
  directive_attribute *class_attr = NULL;
  int first = 1;

  if (directive->attributes_json.data)
    return (const char *)directive->attributes_json.data;

  cmark_strbuf_init(cmark_node_mem(node), &json, 0);
  cmark_strbuf_putc(&json, '{');
  for (attr = directive->attributes; attr; attr = attr->next) {
    if (attribute_name_equals(attr, (const unsigned char *)"id", 2)) {
      id_attr = attr;
    } else if (attribute_name_equals(attr, (const unsigned char *)"class",
                                    5)) {
      class_attr = attr;
    }
  }
  if (id_attr)
    render_one_json_attribute(&json, id_attr, &first);
  if (class_attr)
    render_one_json_attribute(&json, class_attr, &first);
  for (attr = directive->attributes; attr; attr = attr->next) {
    if (!attribute_name_equals(attr, (const unsigned char *)"id", 2) &&
        !attribute_name_equals(attr, (const unsigned char *)"class", 5))
      render_one_json_attribute(&json, attr, &first);
  }
  cmark_strbuf_putc(&json, '}');

  directive->attributes_json = cmark_chunk_buf_detach(&json);
  return (const char *)directive->attributes_json.data;
}

static void skip_json_space(const unsigned char *data, bufsize_t len,
                            bufsize_t *pos) {
  while (*pos < len && ascii_is_space(data[*pos]))
    (*pos)++;
}

static int json_hex_value(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static int parse_json_hex4(const unsigned char *data, bufsize_t len,
                           bufsize_t *pos, int32_t *codepoint) {
  int i;
  int32_t value = 0;

  if (*pos + 4 > len)
    return 0;

  for (i = 0; i < 4; i++) {
    int digit = json_hex_value(data[*pos + (bufsize_t)i]);
    if (digit < 0)
      return 0;
    value = (value << 4) | digit;
  }

  *pos += 4;
  *codepoint = value;
  return 1;
}

static int parse_json_unicode_escape(const unsigned char *data, bufsize_t len,
                                     bufsize_t *pos, cmark_strbuf *buf) {
  int32_t codepoint;

  if (!parse_json_hex4(data, len, pos, &codepoint))
    return 0;

  if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
    int32_t low;
    if (*pos + 2 > len || data[*pos] != '\\' || data[*pos + 1] != 'u')
      return 0;
    *pos += 2;
    if (!parse_json_hex4(data, len, pos, &low) || low < 0xDC00 ||
        low > 0xDFFF)
      return 0;
    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
  } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
    return 0;
  }

  cmark_utf8proc_encode_char(codepoint, buf);
  return 1;
}

static int parse_json_string(cmark_mem *mem, const unsigned char *data,
                             bufsize_t len, bufsize_t *pos,
                             cmark_chunk *result) {
  cmark_strbuf value;

  if (*pos >= len || data[*pos] != '"')
    return 0;

  (*pos)++;
  cmark_strbuf_init(mem, &value, 0);
  while (*pos < len) {
    unsigned char c = data[*pos];
    (*pos)++;

    if (c == '"') {
      *result = cmark_chunk_buf_detach(&value);
      return 1;
    }

    if (c < 0x20) {
      cmark_strbuf_free(&value);
      return 0;
    }

    if (c == '\\') {
      if (*pos >= len) {
        cmark_strbuf_free(&value);
        return 0;
      }

      c = data[*pos];
      (*pos)++;
      switch (c) {
      case '"':
      case '\\':
      case '/':
        cmark_strbuf_putc(&value, c);
        break;
      case 'b':
        cmark_strbuf_putc(&value, '\b');
        break;
      case 'f':
        cmark_strbuf_putc(&value, '\f');
        break;
      case 'n':
        cmark_strbuf_putc(&value, '\n');
        break;
      case 'r':
        cmark_strbuf_putc(&value, '\r');
        break;
      case 't':
        cmark_strbuf_putc(&value, '\t');
        break;
      case 'u':
        if (!parse_json_unicode_escape(data, len, pos, &value)) {
          cmark_strbuf_free(&value);
          return 0;
        }
        break;
      default:
        cmark_strbuf_free(&value);
        return 0;
      }
    } else {
      cmark_strbuf_putc(&value, c);
    }
  }

  cmark_strbuf_free(&value);
  return 0;
}

static int parse_json_null(const unsigned char *data, bufsize_t len,
                           bufsize_t *pos) {
  if (*pos + 4 <= len && memcmp(data + *pos, "null", 4) == 0) {
    *pos += 4;
    return 1;
  }

  return 0;
}

static int parse_attributes_json(cmark_mem *mem, const unsigned char *data,
                                 bufsize_t len,
                                 directive_attribute **result) {
  directive_attribute *attrs = NULL;
  directive_attribute *tail = NULL;
  bufsize_t pos = 0;
  int ok = 0;

#define FAIL_JSON_ATTRIBUTE                                                     \
  do {                                                                         \
    cmark_chunk_free(mem, &name);                                              \
    cmark_chunk_free(mem, &value);                                             \
    goto done;                                                                 \
  } while (0)

  *result = NULL;
  skip_json_space(data, len, &pos);
  if (pos >= len || data[pos] != '{')
    return 0;
  pos++;
  skip_json_space(data, len, &pos);

  if (pos < len && data[pos] == '}') {
    pos++;
    skip_json_space(data, len, &pos);
    return pos == len;
  }

  while (pos < len) {
    cmark_chunk name = CMARK_CHUNK_EMPTY;
    cmark_chunk value = CMARK_CHUNK_EMPTY;
    int is_null = 0;

    if (!parse_json_string(mem, data, len, &pos, &name))
      FAIL_JSON_ATTRIBUTE;
    if (!attribute_name_is_valid(name.data, name.len))
      FAIL_JSON_ATTRIBUTE;

    skip_json_space(data, len, &pos);
    if (pos >= len || data[pos] != ':')
      FAIL_JSON_ATTRIBUTE;
    pos++;
    skip_json_space(data, len, &pos);

    if (parse_json_null(data, len, &pos)) {
      is_null = 1;
      remove_attribute(mem, &attrs, &tail, name.data, name.len);
    } else if (!parse_json_string(mem, data, len, &pos, &value)) {
      FAIL_JSON_ATTRIBUTE;
    }

    if (!is_null &&
        !set_or_append_attribute(mem, &attrs, &tail, name.data, name.len,
                                 value.data, value.len))
      FAIL_JSON_ATTRIBUTE;

    cmark_chunk_free(mem, &name);
    cmark_chunk_free(mem, &value);

    skip_json_space(data, len, &pos);
    if (pos < len && data[pos] == ',') {
      pos++;
      skip_json_space(data, len, &pos);
      continue;
    }

    if (pos < len && data[pos] == '}') {
      pos++;
      skip_json_space(data, len, &pos);
      ok = pos == len;
      break;
    }

    FAIL_JSON_ATTRIBUTE;
  }

done:
#undef FAIL_JSON_ATTRIBUTE
  if (ok) {
    *result = attrs;
  } else {
    free_attribute_list(mem, attrs);
  }
  return ok;
}

const char *cmark_gfm_extensions_get_directive_name(cmark_node *node) {
  node_directive *directive = get_directive(node);
  if (!directive)
    return NULL;

  return cmark_chunk_to_cstr(cmark_node_mem(node), &directive->name);
}

static int directive_name_is_valid(cmark_mem *mem, const char *name) {
  size_t raw_len;
  unsigned char *copy;
  bufsize_t len;
  bufsize_t name_start;
  bufsize_t name_len;
  int valid;

  if (!name)
    return 0;

  raw_len = strlen(name);
  if (raw_len == 0 || raw_len > INT_MAX)
    return 0;

  len = (bufsize_t)raw_len;
  copy = (unsigned char *)mem->calloc((size_t)len + 1, 1);
  if (!copy)
    return 0;

  memcpy(copy, name, (size_t)len);
  valid = scan_name(copy, len, 0, &name_start, &name_len) &&
          name_start == 0 && name_len == len;
  mem->free(copy);
  return valid;
}

int cmark_gfm_extensions_set_directive_name(cmark_node *node,
                                            const char *name) {
  node_directive *directive = get_directive(node);

  if (!directive || !directive_name_is_valid(cmark_node_mem(node), name))
    return 0;

  cmark_chunk_set_cstr(cmark_node_mem(node), &directive->name, name);
  clear_xml_attr(node, directive);
  return 1;
}

const char *
cmark_gfm_extensions_get_directive_attributes(cmark_node *node) {
  node_directive *directive = get_directive(node);
  if (!directive)
    return NULL;

  return render_attributes_json(node, directive);
}

int cmark_gfm_extensions_set_directive_attributes(
    cmark_node *node, const char *attributes) {
  node_directive *directive = get_directive(node);
  directive_attribute *parsed_attributes = NULL;
  if (!directive || !attributes)
    return 0;

  if (!parse_attributes_json(cmark_node_mem(node),
                             (const unsigned char *)attributes,
                             (bufsize_t)strlen(attributes),
                             &parsed_attributes))
    return 0;

  free_attribute_list(cmark_node_mem(node), directive->attributes);
  directive->attributes = parsed_attributes;
  directive->has_attributes = 1;
  clear_attribute_caches(node, directive);
  return 1;
}

static void directive_opaque_alloc(cmark_syntax_extension *extension,
                                           cmark_mem *mem, cmark_node *node) {
  if (is_directive_node(node))
    node->as.opaque = mem->calloc(1, sizeof(node_directive));
}

static void directive_opaque_free(cmark_syntax_extension *extension,
                                          cmark_mem *mem, cmark_node *node) {
  node_directive *directive = (node_directive *)node->as.opaque;
  if (!directive)
    return;

  cmark_chunk_free(mem, &directive->name);
  free_attribute_list(mem, directive->attributes);
  cmark_chunk_free(mem, &directive->attributes_json);
  cmark_chunk_free(mem, &directive->xml_attr);
  mem->free(directive);
}

static void append_escaped(cmark_strbuf *buf, const unsigned char *data,
                           bufsize_t len) {
  houdini_escape_html0(buf, data, len, 0);
}

static void append_class_value(cmark_strbuf *classes, const unsigned char *data,
                               bufsize_t len) {
  bufsize_t pos = 0;

  while (pos < len) {
    bufsize_t start;

    while (pos < len && ascii_is_space(data[pos]))
      pos++;

    start = pos;
    while (pos < len && !ascii_is_space(data[pos]))
      pos++;

    if (pos > start) {
      if (classes->size)
        cmark_strbuf_putc(classes, ' ');
      cmark_strbuf_put(classes, data + start, pos - start);
    }
  }
}

static int is_attr_name_char(unsigned char c) {
  return c > 0x20 && c != '=' && c != '"' && c != '\'' && c != '<' &&
         c != '>' && c != '/' && c != '{' && c != '}';
}

static int attr_name_matches(const unsigned char *data, bufsize_t len,
                             const char *name) {
  return strlen(name) == len && memcmp(data, name, len) == 0;
}

static int attr_name_case_matches(const unsigned char *data, bufsize_t len,
                                  const char *name) {
  bufsize_t i;

  if (strlen(name) != len)
    return 0;

  for (i = 0; i < len; i++) {
    if (ascii_lower(data[i]) != ascii_lower((unsigned char)name[i]))
      return 0;
  }

  return 1;
}

static int attr_name_case_starts_with(const unsigned char *data, bufsize_t len,
                                      const char *prefix) {
  bufsize_t i;
  bufsize_t prefix_len = (bufsize_t)strlen(prefix);

  if (len < prefix_len)
    return 0;

  for (i = 0; i < prefix_len; i++) {
    if (ascii_lower(data[i]) != ascii_lower((unsigned char)prefix[i]))
      return 0;
  }

  return 1;
}

static int parse_attr_value(const unsigned char *data, bufsize_t len,
                            bufsize_t *pos, const unsigned char **value,
                            bufsize_t *value_len) {
  bufsize_t start;
  unsigned char quote;

  while (*pos < len && ascii_is_space(data[*pos]))
    (*pos)++;

  if (*pos >= len || ascii_is_space(data[*pos])) {
    *value = data + *pos;
    *value_len = 0;
    return 1;
  }

  if (data[*pos] == '"' || data[*pos] == '\'') {
    quote = data[*pos];
    (*pos)++;
    start = *pos;
    while (*pos < len && data[*pos] != quote)
      (*pos)++;
    if (*pos >= len)
      return 0;
    *value = data + start;
    *value_len = *pos - start;
    (*pos)++;
    return 1;
  }

  start = *pos;
  while (*pos < len && !ascii_is_space(data[*pos]))
    (*pos)++;

  *value = data + start;
  *value_len = *pos - start;
  return 1;
}

static int parse_attributes(cmark_mem *mem, const unsigned char *data,
                            bufsize_t len, directive_attribute **result) {
  cmark_strbuf id;
  cmark_strbuf classes;
  directive_attribute *attrs = NULL;
  directive_attribute *tail = NULL;
  directive_attribute *final_attrs = NULL;
  directive_attribute *final_tail = NULL;
  bufsize_t pos = 0;
  int has_id = 0;
  int has_class = 0;
  int ok = 1;

  *result = NULL;
  cmark_strbuf_init(mem, &id, 0);
  cmark_strbuf_init(mem, &classes, 0);

  while (pos < len) {
    bufsize_t start;
    const unsigned char *value = (const unsigned char *)"";
    bufsize_t value_len = 0;

    while (pos < len && ascii_is_space(data[pos]))
      pos++;

    if (pos >= len)
      break;

    if (data[pos] == '#') {
      pos++;
      start = pos;
      while (pos < len && !ascii_is_space(data[pos]))
        pos++;
      if (pos == start) {
        ok = 0;
        break;
      }
      cmark_strbuf_clear(&id);
      cmark_strbuf_put(&id, data + start, pos - start);
      has_id = 1;
      continue;
    }

    if (data[pos] == '.') {
      pos++;
      start = pos;
      while (pos < len && !ascii_is_space(data[pos]))
        pos++;
      if (pos == start) {
        ok = 0;
        break;
      }
      append_class_value(&classes, data + start, pos - start);
      has_class = 1;
      continue;
    }

    start = pos;
    while (pos < len && is_attr_name_char(data[pos]))
      pos++;

    if (pos == start) {
      ok = 0;
      break;
    }

    bufsize_t name_len = pos - start;

    while (pos < len && ascii_is_space(data[pos]))
      pos++;

    if (pos < len && data[pos] == '=') {
      pos++;
      if (!parse_attr_value(data, len, &pos, &value, &value_len)) {
        ok = 0;
        break;
      }
    }

    if (attr_name_matches(data + start, name_len, "id")) {
      cmark_strbuf_clear(&id);
      cmark_strbuf_put(&id, value, value_len);
      has_id = 1;
    } else if (attr_name_matches(data + start, name_len, "class")) {
      append_class_value(&classes, value, value_len);
      has_class = 1;
    } else {
      if (!set_or_append_attribute(mem, &attrs, &tail, data + start, name_len,
                                   value, value_len)) {
        ok = 0;
        break;
      }
    }
  }

  if (ok) {
    if (has_id &&
        !set_or_append_attribute(mem, &final_attrs, &final_tail,
                                 (const unsigned char *)"id", 2, id.ptr,
                                 id.size)) {
      ok = 0;
    }

    if (ok && has_class &&
        !set_or_append_attribute(mem, &final_attrs, &final_tail,
                                 (const unsigned char *)"class", 5,
                                 classes.ptr, classes.size)) {
      ok = 0;
    }

    if (ok) {
      if (final_tail) {
        final_tail->next = attrs;
      } else {
        final_attrs = attrs;
      }
      attrs = NULL;
      *result = final_attrs;
      final_attrs = NULL;
    }
  }

  free_attribute_list(mem, attrs);
  free_attribute_list(mem, final_attrs);
  cmark_strbuf_free(&id);
  cmark_strbuf_free(&classes);
  return ok;
}

static void free_parsed_directive(cmark_mem *mem, parsed_directive *parsed) {
  free_attribute_list(mem, parsed->attributes);
}

static int parse_directive_suffix(cmark_mem *mem, unsigned char *data,
                                  bufsize_t len, bufsize_t pos,
                                  parsed_directive *parsed) {
  bufsize_t attr_start;
  bufsize_t attr_len;

  memset(parsed, 0, sizeof(*parsed));

  if (!scan_name(data, len, pos, &parsed->name_start, &parsed->name_len))
    return 0;

  pos = parsed->name_start + parsed->name_len;

  if (pos < len && data[pos] == '[') {
    parsed->has_label = 1;
    if (!scan_label(data, len, pos, &parsed->label_start, &parsed->label_len,
                    &pos))
      return 0;
  }

  if (pos < len && data[pos] == '{') {
    parsed->has_attributes = 1;
    if (!scan_attributes_raw(data, len, pos, &attr_start, &attr_len, &pos))
      return 0;
    if (!parse_attributes(mem, data + attr_start, attr_len,
                          &parsed->attributes))
      return 0;
  }

  parsed->end = pos;
  return 1;
}

static cmark_node *make_label_node(cmark_syntax_extension *extension,
                                   cmark_mem *mem, const unsigned char *label,
                                   bufsize_t label_len, int start_line,
                                   int start_column, int end_column) {
  cmark_node *label_node = cmark_node_new_with_mem_and_ext(
      CMARK_NODE_DIRECTIVE_LABEL, mem, extension);
  if (!label_node)
    return NULL;

  cmark_strbuf_put(&label_node->content, label, label_len);
  label_node->start_line = label_node->end_line = start_line;
  label_node->start_column = start_column;
  label_node->end_column = end_column;
  return label_node;
}

static int attach_label_node(cmark_syntax_extension *extension,
                             cmark_node *directive_node,
                             const unsigned char *label, bufsize_t label_len,
                             int start_line, int start_column, int end_column) {
  cmark_node *label_node;

  label_node = make_label_node(extension, cmark_node_mem(directive_node), label,
                               label_len, start_line, start_column, end_column);
  if (!label_node)
    return 0;

  if (!cmark_node_append_child(directive_node, label_node)) {
    cmark_node_free(label_node);
    return 0;
  }

  return 1;
}

static int apply_parsed_directive(cmark_syntax_extension *extension,
                                  cmark_node *node, const unsigned char *data,
                                  parsed_directive *parsed, int start_line,
                                  int start_column) {
  node_directive *directive = get_directive(node);
  cmark_mem *mem = cmark_node_mem(node);

  if (!directive)
    return 0;

  set_chunk_bytes(mem, &directive->name, data + parsed->name_start,
                  parsed->name_len);
  directive->has_label = parsed->has_label;
  directive->has_attributes = parsed->has_attributes;

  if (parsed->has_attributes) {
    free_attribute_list(mem, directive->attributes);
    directive->attributes = parsed->attributes;
    parsed->attributes = NULL;
    clear_attribute_caches(node, directive);
  }

  if (parsed->has_label) {
    int label_start_column = start_column + (int)parsed->label_start + 1;
    int label_end_column = label_start_column + (int)parsed->label_len - 1;
    if (parsed->label_len == 0)
      label_end_column = label_start_column - 1;

    if (!attach_label_node(extension, node, data + parsed->label_start,
                           parsed->label_len, start_line, label_start_column,
                           label_end_column))
      return 0;
  }

  return 1;
}

static cmark_node *make_inline_directive_node(cmark_syntax_extension *extension,
                                              cmark_parser *parser,
                                              const unsigned char *name,
                                              bufsize_t name_len,
                                              int start_line,
                                              int start_column,
                                              int end_line,
                                              int end_column) {
  cmark_node *node = cmark_node_new_with_mem_and_ext(
      CMARK_NODE_DIRECTIVE_INLINE, parser->mem, extension);
  node_directive *directive;

  if (!node)
    return NULL;

  directive = get_directive(node);
  set_chunk_bytes(parser->mem, &directive->name, name, name_len);
  node->start_line = start_line;
  node->end_line = end_line;
  node->start_column = start_column;
  node->end_column = end_column;
  return node;
}

static cmark_node *
make_name_only_inline_directive(cmark_syntax_extension *extension,
                                cmark_parser *parser,
                                cmark_inline_parser *inline_parser,
                                const unsigned char *name,
                                bufsize_t name_len, bufsize_t end_offset) {
  cmark_node *node;
  int start_line = cmark_inline_parser_get_line(inline_parser);
  int start_column = cmark_inline_parser_get_column(inline_parser);
  bufsize_t offset = (bufsize_t)cmark_inline_parser_get_offset(inline_parser);

  node = make_inline_directive_node(
      extension, parser, name, name_len, start_line, start_column, start_line,
      start_column + (int)(end_offset - offset) - 1);
  if (node)
    cmark_inline_parser_set_offset(inline_parser, (int)end_offset);

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

static cmark_node *match_directive_delimiter(
    cmark_parser *parser, cmark_inline_parser *inline_parser,
    unsigned char delim_char, bufsize_t offset, bufsize_t len, int can_open,
    int can_close) {
  cmark_node *node = make_delimiter_text(parser, inline_parser, offset, len);

  if (!node)
    return NULL;

  cmark_inline_parser_push_delimiter(inline_parser, delim_char, can_open,
                                     can_close, node);
  return node;
}

static delimiter *find_directive_opener(cmark_inline_parser *inline_parser,
                                        unsigned char delim_char) {
  delimiter *delim = cmark_inline_parser_get_last_delimiter(inline_parser);
  int closer_count = 0;

  while (delim) {
    if (delim->delim_char == delim_char) {
      if (delim->can_close) {
        closer_count++;
      } else if (delim->can_open) {
        if (closer_count > 0)
          closer_count--;
        else
          return delim;
      }
    }
    delim = delim->previous;
  }

  return NULL;
}

static int scan_parsed_attributes(cmark_mem *mem, const unsigned char *data,
                                  bufsize_t len, bufsize_t pos,
                                  bufsize_t *end) {
  directive_attribute *attributes = NULL;
  bufsize_t attr_start;
  bufsize_t attr_len;

  if (!scan_attributes_raw(data, len, pos, &attr_start, &attr_len, end))
    return 0;

  if (!parse_attributes(mem, data + attr_start, attr_len, &attributes))
    return 0;

  free_attribute_list(mem, attributes);
  return 1;
}

static cmark_node *match_colon_directive(cmark_syntax_extension *extension,
                                         cmark_parser *parser,
                                         cmark_inline_parser *inline_parser,
                                         cmark_chunk *chunk,
                                         bufsize_t offset) {
  bufsize_t name_start;
  bufsize_t name_len;
  bufsize_t pos;

  if (offset + 1 >= chunk->len || chunk->data[offset + 1] == ':')
    return NULL;

  if (!scan_name(chunk->data, chunk->len, offset + 1, &name_start, &name_len))
    return NULL;

  pos = name_start + name_len;
  if (pos < chunk->len && chunk->data[pos] == '[') {
    return match_directive_delimiter(
        parser, inline_parser, DIRECTIVE_LABEL_DELIM, offset,
        pos - offset + 1, 1, 0);
  }

  if (pos < chunk->len && chunk->data[pos] == '{') {
    return match_directive_delimiter(
        parser, inline_parser, DIRECTIVE_ATTR_DELIM, offset,
        pos - offset + 1, 1, 0);
  }

  if (pos < chunk->len && chunk->data[pos] == ':')
    return NULL;

  return make_name_only_inline_directive(extension, parser, inline_parser,
                                         chunk->data + name_start, name_len,
                                         pos);
}

static cmark_node *match_label_closer(cmark_parser *parser,
                                      cmark_inline_parser *inline_parser,
                                      cmark_chunk *chunk, bufsize_t offset) {
  delimiter *opener =
      find_directive_opener(inline_parser, DIRECTIVE_LABEL_DELIM);
  bufsize_t end;
  bufsize_t closer_len = 1;

  if (!opener)
    return NULL;

  if (offset + 1 < chunk->len && chunk->data[offset + 1] == '{' &&
      scan_parsed_attributes(parser->mem, chunk->data, chunk->len,
                             offset + 1, &end)) {
    closer_len = end - offset;
  }

  return match_directive_delimiter(parser, inline_parser,
                                   DIRECTIVE_LABEL_DELIM, offset,
                                   closer_len, 0, 1);
}

static cmark_node *match_attribute_closer(cmark_parser *parser,
                                          cmark_inline_parser *inline_parser,
                                          bufsize_t offset) {
  delimiter *opener =
      find_directive_opener(inline_parser, DIRECTIVE_ATTR_DELIM);

  if (!opener)
    return NULL;

  return match_directive_delimiter(parser, inline_parser,
                                   DIRECTIVE_ATTR_DELIM, offset, 1, 0,
                                   1);
}

static cmark_node *match(cmark_syntax_extension *extension,
                         cmark_parser *parser, cmark_node *parent,
                         unsigned char character,
                         cmark_inline_parser *inline_parser) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  bufsize_t offset = (bufsize_t)cmark_inline_parser_get_offset(inline_parser);

  if (!directive_enabled(parser))
    return NULL;

  if (character == ':')
    return match_colon_directive(extension, parser, inline_parser, chunk,
                                 offset);

  if (character == ']')
    return match_label_closer(parser, inline_parser, chunk, offset);

  if (character == '}')
    return match_attribute_closer(parser, inline_parser, offset);

  return NULL;
}

static bufsize_t count_colons(const unsigned char *data, bufsize_t len,
                              bufsize_t pos) {
  bufsize_t count = 0;
  while (pos + count < len && data[pos + count] == ':')
    count++;
  return count;
}

static cmark_node *open_directive_block(cmark_syntax_extension *extension,
                                        int indented, cmark_parser *parser,
                                        cmark_node *parent_container,
                                        unsigned char *input, int len) {
  bufsize_t first_nonspace = (bufsize_t)cmark_parser_get_first_nonspace(parser);
  bufsize_t colon_count;
  parsed_directive parsed;
  cmark_node *node;
  node_directive *directive;

  if (!directive_enabled(parser) || indented)
    return NULL;

  colon_count = count_colons(input, (bufsize_t)len, first_nonspace);
  if (colon_count < 2)
    return NULL;

  if (!parse_directive_suffix(parser->mem, input, (bufsize_t)len,
                              first_nonspace + colon_count, &parsed))
    return NULL;

  if (!has_only_spaces_until_line_end(input, (bufsize_t)len, parsed.end)) {
    free_parsed_directive(parser->mem, &parsed);
    return NULL;
  }

  node = cmark_parser_add_child(parser, parent_container,
                                CMARK_NODE_DIRECTIVE_BLOCK,
                                (int)first_nonspace + 1);
  if (!node) {
    free_parsed_directive(parser->mem, &parsed);
    return NULL;
  }

  cmark_node_set_syntax_extension(node, extension);
  node->as.opaque = parser->mem->calloc(1, sizeof(node_directive));

  if (!apply_parsed_directive(extension, node, input, &parsed,
                              cmark_parser_get_line_number(parser),
                              (int)first_nonspace)) {
    cmark_node_free(node);
    free_parsed_directive(parser->mem, &parsed);
    return NULL;
  }

  directive = get_directive(node);
  directive->fence_length = (int)colon_count;
  directive->closed = (colon_count == 2);
  directive->consume_line = 1;

  cmark_parser_advance_offset(parser, (char *)input,
                              len - cmark_parser_get_offset(parser), false);

  free_parsed_directive(parser->mem, &parsed);
  return node;
}

static int directive_block_matches(cmark_syntax_extension *extension,
                                   cmark_parser *parser, unsigned char *input,
                                   int len, cmark_node *container) {
  node_directive *directive = get_directive(container);
  bufsize_t first_nonspace = (bufsize_t)cmark_parser_get_first_nonspace(parser);
  bufsize_t colon_count;

  if (!directive)
    return 0;

  if (directive->closed)
    return 0;

  directive->consume_line = 0;

  colon_count = count_colons(input, (bufsize_t)len, first_nonspace);
  if (cmark_parser_get_indent(parser) <= 3 &&
      colon_count >= (bufsize_t)directive->fence_length &&
      has_only_spaces_until_line_end(input, (bufsize_t)len,
                                     first_nonspace + colon_count)) {
    directive->closed = 1;
    directive->consume_line = 1;
    cmark_parser_advance_offset(parser, (char *)input,
                                len - cmark_parser_get_offset(parser), false);
  }

  return 1;
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

static int set_parsed_attributes(cmark_node *node, const unsigned char *data,
                                 bufsize_t len) {
  node_directive *directive = get_directive(node);
  directive_attribute *attributes = NULL;

  if (!directive)
    return 0;

  if (!parse_attributes(cmark_node_mem(node), data, len, &attributes))
    return 0;

  free_attribute_list(cmark_node_mem(node), directive->attributes);
  directive->attributes = attributes;
  directive->has_attributes = 1;
  clear_attribute_caches(node, directive);
  return 1;
}

static int set_attributes_from_wrapper(cmark_node *node,
                                       const unsigned char *data,
                                       bufsize_t len, bufsize_t pos) {
  bufsize_t attr_start;
  bufsize_t attr_len;
  bufsize_t end;

  if (!scan_attributes_raw(data, len, pos, &attr_start, &attr_len, &end) ||
      end != len)
    return 0;

  return set_parsed_attributes(node, data + attr_start, attr_len);
}

static cmark_node *make_empty_label_node(cmark_syntax_extension *extension,
                                         cmark_mem *mem, int start_line,
                                         int start_column, int end_line,
                                         int end_column) {
  cmark_node *label_node = cmark_node_new_with_mem_and_ext(
      CMARK_NODE_DIRECTIVE_LABEL, mem, extension);

  if (!label_node)
    return NULL;

  label_node->start_line = start_line;
  label_node->end_line = end_line;
  label_node->start_column = start_column;
  label_node->end_column = end_column;
  return label_node;
}

static delimiter *insert_label_directive(cmark_syntax_extension *extension,
                                         cmark_parser *parser,
                                         cmark_inline_parser *inline_parser,
                                         delimiter *opener,
                                         delimiter *closer) {
  cmark_node *opener_node = opener->inl_text;
  cmark_node *closer_node = closer->inl_text;
  cmark_chunk *opener_literal = &opener_node->as.literal;
  cmark_chunk *closer_literal = &closer_node->as.literal;
  delimiter *res = closer->next;
  cmark_node *directive_node;
  cmark_node *label_node;
  cmark_node *tmp;
  cmark_node *tmpnext;
  node_directive *directive;
  bufsize_t name_len;

  if (opener->delim_char != closer->delim_char ||
      opener_literal->len < 3 ||
      opener_literal->data[0] != ':' ||
      opener_literal->data[opener_literal->len - 1] != '[' ||
      closer_literal->len < 1 || closer_literal->data[0] != ']')
    goto done;

  name_len = opener_literal->len - 2;
  directive_node = make_inline_directive_node(
      extension, parser, opener_literal->data + 1, name_len,
      opener_node->start_line, opener_node->start_column,
      closer_node->end_line, closer_node->end_column);
  if (!directive_node)
    goto done;

  directive = get_directive(directive_node);
  directive->has_label = 1;

  if (closer_literal->len > 1 && closer_literal->data[1] == '{' &&
      !set_attributes_from_wrapper(directive_node, closer_literal->data,
                                   closer_literal->len, 1)) {
    cmark_node_free(directive_node);
    goto done;
  }

  label_node = make_empty_label_node(
      extension, parser->mem, opener_node->end_line,
      opener_node->end_column + 1, closer_node->start_line,
      closer_node->start_column - 1);
  if (!label_node) {
    cmark_node_free(directive_node);
    goto done;
  }

  tmp = opener_node->next;
  while (tmp && tmp != closer_node) {
    tmpnext = tmp->next;
    cmark_node_unlink(tmp);
    cmark_node_append_child(label_node, tmp);
    tmp = tmpnext;
  }

  cmark_node_append_child(directive_node, label_node);

  if (cmark_node_insert_before(opener_node, directive_node)) {
    cmark_node_free(opener_node);
    cmark_node_free(closer_node);
  } else {
    cmark_node_free(directive_node);
  }

done:
  remove_delimiters(inline_parser, opener, closer);
  return res;
}

static delimiter *insert_attribute_directive(cmark_syntax_extension *extension,
                                             cmark_parser *parser,
                                             cmark_inline_parser *inline_parser,
                                             delimiter *opener,
                                             delimiter *closer) {
  cmark_chunk *chunk = cmark_inline_parser_get_chunk(inline_parser);
  cmark_node *opener_node = opener->inl_text;
  cmark_node *closer_node = closer->inl_text;
  cmark_chunk *opener_literal = &opener_node->as.literal;
  delimiter *res = closer->next;
  cmark_node *directive_node;
  bufsize_t name_len;
  bufsize_t body_start = opener->position;
  bufsize_t body_end = closer->position - closer->length;

  if (opener->delim_char != closer->delim_char ||
      opener_literal->len < 3 ||
      opener_literal->data[0] != ':' ||
      opener_literal->data[opener_literal->len - 1] != '{' ||
      body_end < body_start)
    goto done;

  name_len = opener_literal->len - 2;
  directive_node = make_inline_directive_node(
      extension, parser, opener_literal->data + 1, name_len,
      opener_node->start_line, opener_node->start_column,
      closer_node->end_line, closer_node->end_column);
  if (!directive_node)
    goto done;

  if (!set_parsed_attributes(directive_node, chunk->data + body_start,
                             body_end - body_start)) {
    cmark_node_free(directive_node);
    goto done;
  }

  if (cmark_node_insert_before(opener_node, directive_node)) {
    free_nodes_through(opener_node, closer_node);
  } else {
    cmark_node_free(directive_node);
  }

done:
  remove_delimiters(inline_parser, opener, closer);
  return res;
}

static delimiter *insert_directive(cmark_syntax_extension *extension,
                                   cmark_parser *parser,
                                   cmark_inline_parser *inline_parser,
                                   delimiter *opener, delimiter *closer) {
  if (opener->delim_char == DIRECTIVE_LABEL_DELIM)
    return insert_label_directive(extension, parser, inline_parser, opener,
                                  closer);

  if (opener->delim_char == DIRECTIVE_ATTR_DELIM)
    return insert_attribute_directive(extension, parser, inline_parser, opener,
                                      closer);

  return closer->next;
}

static int directive_has_block_children(cmark_node *node) {
  cmark_node *child;

  for (child = node->first_child; child; child = child->next) {
    if (child->type != CMARK_NODE_DIRECTIVE_LABEL &&
        CMARK_NODE_TYPE_BLOCK_P((cmark_node_type)child->type))
      return 1;
  }

  return 0;
}

static void render_directive_name(cmark_strbuf *html,
                                  node_directive *directive) {
  append_escaped(html, directive->name.data, directive->name.len);
}

static int is_safe_html_attr_name(const unsigned char *name,
                                  bufsize_t name_len) {
  return attr_name_case_matches(name, name_len, "id") ||
         attr_name_case_matches(name, name_len, "class") ||
         attr_name_case_starts_with(name, name_len, "data-");
}

static void render_one_html_attr(cmark_strbuf *html, const unsigned char *name,
                                 bufsize_t name_len,
                                 const unsigned char *value,
                                 bufsize_t value_len) {
  cmark_strbuf_putc(html, ' ');
  cmark_strbuf_put(html, name, name_len);
  cmark_strbuf_puts(html, "=\"");
  append_escaped(html, value, value_len);
  cmark_strbuf_putc(html, '"');
}

static void render_html_attrs(cmark_strbuf *html,
                              node_directive *directive, int options) {
  directive_attribute *attr;

  for (attr = directive->attributes; attr; attr = attr->next) {
    if (!attribute_name_is_valid(attr->name.data, attr->name.len))
      continue;

    if ((options & CMARK_OPT_UNSAFE) ||
        is_safe_html_attr_name(attr->name.data, attr->name.len)) {
      render_one_html_attr(html, attr->name.data, attr->name.len,
                           attr->value.data, attr->value.len);
    }
  }
}

static void html_render(cmark_syntax_extension *extension,
                        cmark_html_renderer *renderer, cmark_node *node,
                        cmark_event_type ev_type, int options) {
  node_directive *directive = get_directive(node);

  if (is_label_node(node))
    return;

  if (!directive)
    return;

  if (node->type == CMARK_NODE_DIRECTIVE_INLINE) {
    if (ev_type == CMARK_EVENT_ENTER) {
      cmark_strbuf_puts(renderer->html, "<span data-directive=\"");
      render_directive_name(renderer->html, directive);
      cmark_strbuf_putc(renderer->html, '"');
      render_html_attrs(renderer->html, directive, options);
      cmark_strbuf_putc(renderer->html, '>');
    } else {
      cmark_strbuf_puts(renderer->html, "</span>");
    }
    return;
  }

  if (ev_type == CMARK_EVENT_ENTER) {
    cmark_html_render_cr(renderer->html);
    cmark_strbuf_puts(renderer->html, "<div data-directive=\"");
    render_directive_name(renderer->html, directive);
    cmark_strbuf_putc(renderer->html, '"');
    render_html_attrs(renderer->html, directive, options);
    cmark_strbuf_putc(renderer->html, '>');
  } else {
    if (directive_has_block_children(node))
      cmark_html_render_cr(renderer->html);
    cmark_strbuf_puts(renderer->html, "</div>\n");
  }
}

static void render_commonmark_attrs(cmark_renderer *renderer, cmark_node *node,
                                    node_directive *directive) {
  directive_attribute *attr;
  int first = 1;

  if (!directive->has_attributes)
    return;

  renderer->out(renderer, node, "{", false, LITERAL);
  for (attr = directive->attributes; attr; attr = attr->next) {
    cmark_strbuf value;

    if (!first)
      renderer->out(renderer, node, " ", false, LITERAL);
    first = 0;

    renderer->out(renderer, node,
                  cmark_chunk_to_cstr(renderer->mem, &attr->name), false,
                  LITERAL);
    renderer->out(renderer, node, "=\"", false, LITERAL);
    cmark_strbuf_init(renderer->mem, &value, 0);
    append_escaped(&value, attr->value.data, attr->value.len);
    renderer->out(renderer, node, cmark_strbuf_cstr(&value), false, LITERAL);
    cmark_strbuf_free(&value);
    renderer->out(renderer, node, "\"", false, LITERAL);
  }
  renderer->out(renderer, node, "}", false, LITERAL);
}

static void render_commonmark_opening(cmark_renderer *renderer,
                                      cmark_node *node,
                                      node_directive *directive,
                                      const char *marker) {
  renderer->out(renderer, node, marker, false, LITERAL);
  renderer->out(renderer, node,
                cmark_chunk_to_cstr(renderer->mem, &directive->name), false,
                LITERAL);
  if (!directive->has_label)
    render_commonmark_attrs(renderer, node, directive);
}

static void commonmark_render(cmark_syntax_extension *extension,
                              cmark_renderer *renderer, cmark_node *node,
                              cmark_event_type ev_type, int options) {
  node_directive *directive = get_directive(node);
  cmark_node *parent;
  node_directive *parent_directive;
  char fence[64];
  int i;

  if (is_label_node(node)) {
    parent = cmark_node_parent(node);
    parent_directive = get_directive(parent);

    if (ev_type == CMARK_EVENT_ENTER) {
      renderer->out(renderer, node, "[", false, LITERAL);
    } else {
      renderer->out(renderer, node, "]", false, LITERAL);
      if (parent_directive && parent && parent->type != CMARK_NODE_DIRECTIVE_INLINE) {
        render_commonmark_attrs(renderer, parent, parent_directive);
        if (parent->type == CMARK_NODE_DIRECTIVE_BLOCK &&
            parent_directive->fence_length >= 3)
          renderer->cr(renderer);
      }
    }
    return;
  }

  if (!directive)
    return;

  if (node->type == CMARK_NODE_DIRECTIVE_INLINE) {
    if (ev_type == CMARK_EVENT_ENTER)
      render_commonmark_opening(renderer, node, directive, ":");
    else if (directive->has_label)
      render_commonmark_attrs(renderer, node, directive);
    return;
  }

  if (node->type == CMARK_NODE_DIRECTIVE_BLOCK) {
    if (directive->fence_length == 2) {
      if (ev_type == CMARK_EVENT_ENTER) {
        renderer->blankline(renderer);
        render_commonmark_opening(renderer, node, directive, "::");
      } else {
        renderer->blankline(renderer);
      }
      return;
    }

    if (ev_type == CMARK_EVENT_ENTER) {
      renderer->blankline(renderer);
      if (directive->fence_length < 3)
        directive->fence_length = 3;
      if (directive->fence_length < (int)sizeof(fence)) {
        for (i = 0; i < directive->fence_length; i++)
          fence[i] = ':';
        fence[directive->fence_length] = '\0';
        render_commonmark_opening(renderer, node, directive, fence);
      } else {
        for (i = 0; i < directive->fence_length; i++)
          renderer->out(renderer, node, ":", false, LITERAL);
        renderer->out(renderer, node,
                      cmark_chunk_to_cstr(renderer->mem, &directive->name),
                      false, LITERAL);
        if (!directive->has_label)
          render_commonmark_attrs(renderer, node, directive);
      }
      if (!directive->has_label)
        renderer->cr(renderer);
    } else {
      renderer->blankline(renderer);
      for (i = 0; i < directive->fence_length; i++)
        renderer->out(renderer, node, ":", false, LITERAL);
      renderer->blankline(renderer);
    }
  }
}

static void passthrough_render(cmark_syntax_extension *extension,
                               cmark_renderer *renderer, cmark_node *node,
                               cmark_event_type ev_type, int options) {
  (void)extension;
  (void)renderer;
  (void)node;
  (void)ev_type;
  (void)options;
}

static const char *xml_attr(cmark_syntax_extension *extension,
                            cmark_node *node) {
  node_directive *directive = get_directive(node);
  cmark_strbuf attr;

  if (!directive)
    return NULL;

  if (directive->xml_attr.data)
    return (const char *)directive->xml_attr.data;

  cmark_strbuf_init(cmark_node_mem(node), &attr, 0);
  cmark_strbuf_puts(&attr, " name=\"");
  append_escaped(&attr, directive->name.data, directive->name.len);
  cmark_strbuf_putc(&attr, '"');

  if (directive->has_attributes) {
    const char *json = render_attributes_json(node, directive);
    cmark_strbuf_puts(&attr, " attributes=\"");
    append_escaped(&attr, (const unsigned char *)json,
                   (bufsize_t)strlen(json));
    cmark_strbuf_putc(&attr, '"');
  }

  directive->xml_attr = cmark_chunk_buf_detach(&attr);
  return (const char *)directive->xml_attr.data;
}

static const char *get_type_string(cmark_syntax_extension *extension,
                                   cmark_node *node) {
  if (node->type == CMARK_NODE_DIRECTIVE_INLINE)
    return "directive";

  if (node->type == CMARK_NODE_DIRECTIVE_BLOCK)
    return "directive_container";

  if (node->type == CMARK_NODE_DIRECTIVE_LABEL)
    return "directive_label";

  return "<unknown>";
}

static int can_contain(cmark_syntax_extension *extension, cmark_node *node,
                       cmark_node_type child_type) {
  if (node->type == CMARK_NODE_DIRECTIVE_INLINE)
    return child_type == CMARK_NODE_DIRECTIVE_LABEL;

  if (node->type == CMARK_NODE_DIRECTIVE_BLOCK)
    return child_type == CMARK_NODE_DIRECTIVE_LABEL ||
           (CMARK_NODE_TYPE_BLOCK_P(child_type) &&
            child_type != CMARK_NODE_ITEM && child_type != CMARK_NODE_DOCUMENT);

  if (node->type == CMARK_NODE_DIRECTIVE_LABEL)
    return CMARK_NODE_TYPE_INLINE_P(child_type) &&
           child_type != CMARK_NODE_DIRECTIVE_LABEL;

  return 0;
}

static int contains_inlines(cmark_syntax_extension *extension,
                            cmark_node *node) {
  return node->type == CMARK_NODE_DIRECTIVE_LABEL;
}

static int accepts_lines(cmark_syntax_extension *extension, cmark_node *node) {
  node_directive *directive = get_directive(node);

  if (!directive)
    return 0;

  if (node->type != CMARK_NODE_DIRECTIVE_BLOCK)
    return 0;

  return directive->fence_length == 2 || directive->consume_line;
}

cmark_syntax_extension *create_directive_extension(void) {
  cmark_syntax_extension *ext = cmark_syntax_extension_new("directive");
  cmark_llist *special_chars = NULL;
  cmark_mem *mem = cmark_get_default_mem_allocator();

  CMARK_NODE_DIRECTIVE_INLINE = cmark_syntax_extension_add_node(1);
  CMARK_NODE_DIRECTIVE_BLOCK = cmark_syntax_extension_add_node(0);
  CMARK_NODE_DIRECTIVE_LABEL = cmark_syntax_extension_add_node(1);

  cmark_syntax_extension_set_match_inline_func(ext, match);
  cmark_syntax_extension_set_inline_from_delim_func(ext, insert_directive);
  cmark_syntax_extension_set_match_block_func(ext, directive_block_matches);
  cmark_syntax_extension_set_open_block_func(ext, open_directive_block);
  cmark_syntax_extension_set_get_type_string_func(ext, get_type_string);
  cmark_syntax_extension_set_can_contain_func(ext, can_contain);
  cmark_syntax_extension_set_contains_inlines_func(ext, contains_inlines);
  cmark_syntax_extension_set_accepts_lines_func(ext, accepts_lines);
  cmark_syntax_extension_set_commonmark_render_func(ext, commonmark_render);
  cmark_syntax_extension_set_plaintext_render_func(ext, passthrough_render);
  cmark_syntax_extension_set_latex_render_func(ext, passthrough_render);
  cmark_syntax_extension_set_man_render_func(ext, passthrough_render);
  cmark_syntax_extension_set_html_render_func(ext, html_render);
  cmark_syntax_extension_set_xml_attr_func(ext, xml_attr);
  cmark_syntax_extension_set_opaque_alloc_func(ext,
                                               directive_opaque_alloc);
  cmark_syntax_extension_set_opaque_free_func(ext,
                                              directive_opaque_free);

  special_chars = cmark_llist_append(mem, special_chars, (void *)':');
  special_chars = cmark_llist_append(mem, special_chars, (void *)']');
  special_chars = cmark_llist_append(mem, special_chars, (void *)'}');
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)DIRECTIVE_LABEL_DELIM);
  special_chars = cmark_llist_append(mem, special_chars,
                                     (void *)DIRECTIVE_ATTR_DELIM);
  cmark_syntax_extension_set_special_inline_chars(ext, special_chars);
  cmark_syntax_extension_set_emphasis(ext, 1);

  return ext;
}

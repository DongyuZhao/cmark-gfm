#include "tasklist.h"
#include <parser.h>
#include <render.h>
#include <html.h>
#include <iterator.h>
#include <node.h>
#include <string.h>
#include "ext_scanners.h"

typedef enum {
  CMARK_TASKLIST_NOCHECKED,
  CMARK_TASKLIST_CHECKED,
} cmark_tasklist_type;

// Local constants
static const char *TYPE_STRING = "tasklist";

// Historically the tasklist extension used block-phase callbacks (matches /
// open_tasklist_item) to mark list items and adjust the parser offset. The
// logic below mirrors that behavior in a post-processing pass so downstream
// renderers observe the same node/extension state and source positions.

static const char *get_type_string(cmark_syntax_extension *extension, cmark_node *node) {
  return TYPE_STRING;
}


// Return 1 if state was set, 0 otherwise. These helpers are used by external
// consumers to query or mutate the checked state of a task list item while
// ensuring the node is actually associated with this extension.
int cmark_gfm_extensions_set_tasklist_item_checked(cmark_node *node, bool is_checked) {
  // The node has to exist, and be an extension, and actually be the right type in order to get the value.
  if (!node || !node->extension || strcmp(cmark_node_get_type_string(node), TYPE_STRING))
    return 0;

  node->as.list.checked = is_checked;
  return 1;
}

bool cmark_gfm_extensions_get_tasklist_item_checked(cmark_node *node) {
  if (!node || !node->extension || strcmp(cmark_node_get_type_string(node), TYPE_STRING))
    return false;

  if (node->as.list.checked) {
    return true;
  }
  else {
    return false;
  }
}

static cmark_node *find_first_text_node(cmark_node *block) {
  if (!block) {
    return NULL;
  }

  cmark_iter *iter = cmark_iter_new(block);
  cmark_node *first_text = NULL;
  cmark_event_type ev_type;

  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *cur = cmark_iter_get_node(iter);
    if (ev_type == CMARK_EVENT_ENTER && cur->type == CMARK_NODE_TEXT) {
      first_text = cur;
      break;
    }
  }

  cmark_iter_free(iter);
  return first_text;
}

static bool detect_tasklist_from_ast_item(cmark_node *item, bool *checked_out,
                                          cmark_node **first_text_out,
                                          size_t *prefix_len_out,
                                          size_t *next_ws_out) {
  // Walk the first block and locate the earliest text node, then look for the
  // same task marker pattern the block parser used to recognize. This mirrors
  // the old open_tasklist_item logic while operating on the finished AST.
  if (!item || item->type != CMARK_NODE_ITEM) {
    return false;
  }

  cmark_node *block = item->first_child;
  cmark_node *first_text = find_first_text_node(block);

  if (next_ws_out) {
    *next_ws_out = 0;
  }

  if (!first_text) {
    return false;
  }

  const char *literal = cmark_node_get_literal(first_text);
  if (!literal) {
    return false;
  }

  size_t literal_len = strlen(literal);
  size_t offset = 0;
  while (offset < literal_len && (literal[offset] == ' ' || literal[offset] == '\t')) {
    offset++;
  }

  if (literal_len < offset + 3) {
    return false;
  }

  if (literal[offset] != '[' || literal[offset + 2] != ']') {
    return false;
  }

  char marker = literal[offset + 1];
  if (!(marker == ' ' || marker == 'x' || marker == 'X')) {
    return false;
  }

  size_t trailing = 0;
  size_t trailing_offset = offset + 3;
  while (trailing_offset + trailing < literal_len &&
         (literal[trailing_offset + trailing] == ' ' ||
          literal[trailing_offset + trailing] == '\t')) {
    trailing++;
  }

  size_t leading_ws_next = 0;

  if (trailing == 0) {
    bool marker_at_line_end = (literal_len == offset + 3) &&
                              (item->end_line == first_text->end_line) &&
                              (item->end_column > first_text->end_column);
    bool marker_followed_by_break = first_text->next &&
      (first_text->next->type == CMARK_NODE_SOFTBREAK ||
       first_text->next->type == CMARK_NODE_LINEBREAK);

    if (!marker_at_line_end && !marker_followed_by_break) {
      if (first_text->next && first_text->next->type == CMARK_NODE_TEXT) {
        const char *next_literal = cmark_node_get_literal(first_text->next);
        if (next_literal) {
          while (next_literal[leading_ws_next] == ' ' ||
                 next_literal[leading_ws_next] == '\t') {
            leading_ws_next++;
          }
        }
      }

      if (leading_ws_next == 0) {
        return false;
      }
    }
  }

  if (checked_out) {
    *checked_out = (marker == 'x' || marker == 'X');
  }

  if (first_text_out) {
    *first_text_out = first_text;
  }

  if (prefix_len_out) {
    *prefix_len_out = offset + 3 + trailing;
  }

  if (next_ws_out) {
    *next_ws_out = leading_ws_next;
  }

  return true;
}

static void strip_task_marker(cmark_node *item, cmark_node *first_text,
                              size_t prefix_len, size_t next_ws) {
  if (!first_text) {
    return;
  }

  const char *literal = cmark_node_get_literal(first_text);
  size_t literal_len = literal ? strlen(literal) : 0;
  cmark_node *block = first_text->parent;

  if (!literal || literal_len <= prefix_len) {
    cmark_node *next_block_sibling = NULL;
    cmark_node *next_inline_sibling = first_text->next;
    if (block) {
      next_block_sibling = block->next;
    }

    cmark_node_unlink(first_text);
    cmark_node_free(first_text);

    if (next_ws && next_inline_sibling &&
        next_inline_sibling->type == CMARK_NODE_TEXT) {
      const char *next_lit = cmark_node_get_literal(next_inline_sibling);
      size_t trim = 0;
      if (next_lit) {
        size_t len = strlen(next_lit);
        while (trim < len && trim < next_ws &&
               (next_lit[trim] == ' ' || next_lit[trim] == '\t')) {
          trim++;
        }

        if (trim == len) {
          cmark_node *parent = next_inline_sibling->parent;
          cmark_node *following = next_inline_sibling->next;
          cmark_node_unlink(next_inline_sibling);
          cmark_node_free(next_inline_sibling);

          if (parent && parent->type == CMARK_NODE_PARAGRAPH &&
              parent->first_child == NULL && parent->parent == item &&
              parent->prev == NULL && parent->next == NULL) {
            cmark_node_unlink(parent);
            cmark_node_free(parent);
          }

          next_inline_sibling = following;
        } else if (trim > 0) {
          cmark_node_set_literal(next_inline_sibling, next_lit + trim);
          next_inline_sibling->start_column += (int)trim;
          if (next_inline_sibling->parent &&
              next_inline_sibling->parent->type == CMARK_NODE_PARAGRAPH &&
              next_inline_sibling->parent->first_child == next_inline_sibling) {
            next_inline_sibling->parent->start_column += (int)trim;
          }
        }
      }
    }

    if (block && block->parent == item && block->first_child == NULL &&
        block->prev == NULL && next_block_sibling == NULL) {
      cmark_node_unlink(block);
      cmark_node_free(block);
    }
    return;
  }

  cmark_node_set_literal(first_text, literal + prefix_len);
  first_text->start_column += (int)prefix_len;

  if (block && block->type == CMARK_NODE_PARAGRAPH) {
    block->start_column += (int)prefix_len;
  }
}

// Task list detection now runs as a post-processing pass (mirroring the
// autolink extension). We inspect the completed AST to determine whether a
// list item starts with a task marker, then rewrite the inline text and
// source positions to match the historical block-phase behavior.
static cmark_node *tasklist_postprocess(cmark_syntax_extension *ext,
                                        cmark_parser *parser,
                                        cmark_node *root) {
  (void)parser;

  cmark_iter *iter = cmark_iter_new(root);
  cmark_event_type ev_type;

  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    if (ev_type != CMARK_EVENT_ENTER) {
      continue;
    }

    cmark_node *node = cmark_iter_get_node(iter);
    if (node->type != CMARK_NODE_ITEM) {
      continue;
    }

    bool checked = false;
    cmark_node *first_text = NULL;
    size_t prefix_len = 0;
    size_t next_ws = 0;
    bool is_task = detect_tasklist_from_ast_item(node, &checked, &first_text,
                                                 &prefix_len, &next_ws);

    if (!is_task) {
      continue;
    }

    cmark_node_set_syntax_extension(node, ext);
    node->as.list.checked = checked;
    strip_task_marker(node, first_text, prefix_len, next_ws);
  }

  cmark_iter_free(iter);
  return root;
}

// Renderer callbacks remain unchanged: they rely on the extension marker and
// the stored checked state to emit the appropriate marker text/HTML/XML while
// preserving the surrounding list/item/paragraph structure.
static void commonmark_render(cmark_syntax_extension *extension,
                              cmark_renderer *renderer, cmark_node *node,
                              cmark_event_type ev_type, int options) {
  bool entering = (ev_type == CMARK_EVENT_ENTER);
  if (entering) {
    renderer->cr(renderer);
    if (node->as.list.checked) {
      renderer->out(renderer, node, "- [x] ", false, LITERAL);
    } else {
      renderer->out(renderer, node, "- [ ] ", false, LITERAL);
    }
    cmark_strbuf_puts(renderer->prefix, "  ");
  } else {
    cmark_strbuf_truncate(renderer->prefix, renderer->prefix->size - 2);
    renderer->cr(renderer);
  }
}

static void html_render(cmark_syntax_extension *extension,
                        cmark_html_renderer *renderer, cmark_node *node,
                        cmark_event_type ev_type, int options) {
  bool entering = (ev_type == CMARK_EVENT_ENTER);
  if (entering) {
    cmark_html_render_cr(renderer->html);
    cmark_strbuf_puts(renderer->html, "<li");
    cmark_html_render_sourcepos(node, renderer->html, options);
    cmark_strbuf_putc(renderer->html, '>');
    if (node->as.list.checked) {
      cmark_strbuf_puts(renderer->html, "<input type=\"checkbox\" checked=\"\" disabled=\"\" /> ");
    } else {
      cmark_strbuf_puts(renderer->html, "<input type=\"checkbox\" disabled=\"\" /> ");
    }
  } else {
    cmark_strbuf_puts(renderer->html, "</li>\n");
  }
}

static const char *xml_attr(cmark_syntax_extension *extension,
                            cmark_node *node) {
  if (node->as.list.checked) {
    return " completed=\"true\"";
  } else {
    return " completed=\"false\"";
  }
}

cmark_syntax_extension *create_tasklist_extension(void) {
  cmark_syntax_extension *ext = cmark_syntax_extension_new("tasklist");

  cmark_syntax_extension_set_get_type_string_func(ext, get_type_string);
  cmark_syntax_extension_set_postprocess_func(ext, tasklist_postprocess);
  cmark_syntax_extension_set_commonmark_render_func(ext, commonmark_render);
  cmark_syntax_extension_set_plaintext_render_func(ext, commonmark_render);
  cmark_syntax_extension_set_html_render_func(ext, html_render);
  cmark_syntax_extension_set_xml_attr_func(ext, xml_attr);

  return ext;
}

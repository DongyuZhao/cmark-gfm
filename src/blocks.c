/**
 * Block parsing implementation.
 *
 * For a high-level overview of the block parsing process,
 * see http://spec.commonmark.org/0.24/#phase-1-block-structure
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>

#include "cmark_ctype.h"
#include "syntax_extension.h"
#include "config.h"
#include "parser.h"
#include "cmark-gfm.h"
#include "node.h"
#include "references.h"
#include "utf8.h"
#include "scanners.h"
#include "inlines.h"
#include "houdini.h"
#include "buffer.h"
#include "footnotes.h"
#include "feed.h"

#define CODE_INDENT 4
#define TAB_STOP 4

/**
 * Very deeply nested lists can cause quadratic performance issues.
 * This constant is used in open_new_blocks() to limit the nesting
 * depth. It is unlikely that a non-contrived markdown document will
 * be nested this deeply.
 */
#define MAX_LIST_DEPTH 100

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

#define peek_at(i, n) (i)->data[n]

static bool S_last_line_blank(const cmark_node *node) {
  return (node->flags & CMARK_NODE__LAST_LINE_BLANK) != 0;
}

static bool S_last_line_checked(const cmark_node *node) {
  return (node->flags & CMARK_NODE__LAST_LINE_CHECKED) != 0;
}

static CMARK_INLINE cmark_node_type S_type(const cmark_node *node) {
  return (cmark_node_type)node->type;
}

static void S_set_last_line_blank(cmark_node *node, bool is_blank) {
  if (is_blank)
    node->flags |= CMARK_NODE__LAST_LINE_BLANK;
  else
    node->flags &= ~CMARK_NODE__LAST_LINE_BLANK;
}

static void S_set_last_line_checked(cmark_node *node) {
  node->flags |= CMARK_NODE__LAST_LINE_CHECKED;
}

static CMARK_INLINE bool S_is_line_end_char(char c) {
  return (c == '\n' || c == '\r');
}

static CMARK_INLINE bool S_is_space_or_tab(char c) {
  return (c == ' ' || c == '\t');
}

static void S_parser_feed(cmark_parser *parser, const unsigned char *buffer,
                          size_t len, bool eof);

static void S_process_line(cmark_parser *parser, const unsigned char *buffer,
                           bufsize_t bytes);

static cmark_node *make_block(cmark_mem *mem, cmark_node_type tag,
                              int start_line, int start_column) {
  cmark_node *e;

  e = (cmark_node *)mem->calloc(1, sizeof(*e));
  cmark_strbuf_init(mem, &e->content, 32);
  e->type = (uint16_t)tag;
  e->flags = CMARK_NODE__OPEN;
  e->start_line = start_line;
  e->start_column = start_column;
  e->end_line = start_line;

  return e;
}

// Create a root document node.
static cmark_node *make_document(cmark_mem *mem) {
  cmark_node *e = make_block(mem, CMARK_NODE_DOCUMENT, 1, 1);
  return e;
}

int cmark_parser_attach_syntax_extension(cmark_parser *parser,
                                         cmark_syntax_extension *extension) {
  parser->syntax_extensions = cmark_llist_append(parser->mem, parser->syntax_extensions, extension);
  if (extension->match_inline || extension->insert_inline_from_delim) {
    parser->inline_syntax_extensions = cmark_llist_append(
      parser->mem, parser->inline_syntax_extensions, extension);
  }

  return 1;
}

static void cmark_parser_dispose(cmark_parser *parser) {
  // Feed: free intrusive event/dirty/pending lists *before* the tree
  // so the per-node "forget" hook in S_free_nodes short-circuits — without
  // this, full disposal would pay O(N * P) walking lists for every node.
  cmark_parser_feed_state_free(parser->mem, &parser->feed);

  if (parser->root)
    cmark_node_free(parser->root);

  if (parser->refmap)
    cmark_map_free(parser->refmap);
}

static void cmark_parser_reset(cmark_parser *parser) {
  cmark_llist *saved_exts = parser->syntax_extensions;
  cmark_llist *saved_inline_exts = parser->inline_syntax_extensions;
  int saved_options = parser->options;
  cmark_mem *saved_mem = parser->mem;

  cmark_parser_dispose(parser);

  memset(parser, 0, sizeof(cmark_parser));
  parser->mem = saved_mem;

  cmark_strbuf_init(parser->mem, &parser->curline, 256);
  cmark_strbuf_init(parser->mem, &parser->linebuf, 0);

  cmark_node *document = make_document(parser->mem);

  parser->refmap = cmark_reference_map_new(parser->mem);
  parser->root = document;
  parser->current = document;

  parser->syntax_extensions = saved_exts;
  parser->inline_syntax_extensions = saved_inline_exts;
  parser->options = saved_options;

  cmark_parser_feed_state_init(&parser->feed);
  // Feed mode is now read directly from parser->options & CMARK_OPT_FEED_AST
  // — there is no separate feed_active state to reset. Sticky-across-reset
  // is intrinsic because saved_options is preserved above.
}

cmark_parser *cmark_parser_new_with_mem(int options, cmark_mem *mem) {
  cmark_parser *parser = (cmark_parser *)mem->calloc(1, sizeof(cmark_parser));
  parser->mem = mem;
  parser->options = options;
  cmark_parser_reset(parser);
  return parser;
}

cmark_parser *cmark_parser_new(int options) {
  extern cmark_mem CMARK_DEFAULT_MEM_ALLOCATOR;
  return cmark_parser_new_with_mem(options, &CMARK_DEFAULT_MEM_ALLOCATOR);
}

void cmark_parser_free(cmark_parser *parser) {
  cmark_mem *mem = parser->mem;
  cmark_parser_dispose(parser);
  cmark_strbuf_free(&parser->curline);
  cmark_strbuf_free(&parser->linebuf);
  cmark_llist_free(parser->mem, parser->syntax_extensions);
  cmark_llist_free(parser->mem, parser->inline_syntax_extensions);
  mem->free(parser);
}

static cmark_node *finalize(cmark_parser *parser, cmark_node *b);
static void try_eager_ref_extract(cmark_parser *parser, cmark_node *p);

// Returns true if line has only space characters, else false.
static bool is_blank(cmark_strbuf *s, bufsize_t offset) {
  while (offset < s->size) {
    switch (s->ptr[offset]) {
    case '\r':
    case '\n':
      return true;
    case ' ':
      offset++;
      break;
    case '\t':
      offset++;
      break;
    default:
      return false;
    }
  }

  return true;
}

static CMARK_INLINE bool accepts_lines(cmark_node_type block_type) {
  return (block_type == CMARK_NODE_PARAGRAPH ||
          block_type == CMARK_NODE_HEADING ||
          block_type == CMARK_NODE_CODE_BLOCK);
}

// Declared in parser.h (internal header) so feed.c can share it.
// One definition of "what kinds of blocks have inline children";
// duplicating across translation units is a maintenance hazard (extensions
// adding new block types would need every copy updated).
bool cmark_block_contains_inlines(cmark_node *node) {
  if (node->extension && node->extension->contains_inlines_func) {
    return node->extension->contains_inlines_func(node->extension, node) != 0;
  }

  return (node->type == CMARK_NODE_PARAGRAPH ||
          node->type == CMARK_NODE_HEADING);
}

static void add_line(cmark_node *node, cmark_chunk *ch, cmark_parser *parser) {
  int chars_to_tab;
  int i;
  assert(node->flags & CMARK_NODE__OPEN);
  // Feed: snapshot a partial-line transaction record before mutating
  // node->content so cmark_parser_feed_partial_txn_revert can truncate back.
  // No-op outside an active transaction.
  if (cmark_parser_feed_partial_txn_active(parser)) {
    cmark_parser_feed_partial_txn_record_add_line(parser, node, node->content.size);
  }
  if (parser->partially_consumed_tab) {
    parser->offset += 1; // skip over tab
    // add space characters:
    chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
    for (i = 0; i < chars_to_tab; i++) {
      cmark_strbuf_putc(&node->content, ' ');
    }
  }
  cmark_strbuf_put(&node->content, ch->data + parser->offset,
                   ch->len - parser->offset);

  // Feed-only: try to extract any leading ref-defs that are now
  // complete and bounded. This populates the refmap as soon as possible —
  // without waiting for the paragraph to close — so earlier blocks with
  // [foo] references resolve to LINK on the next snapshot. Strictly gated
  // on feed_active: in oneshot (cmark_parse_document or feed+finish
  // without snapshot), refmap is fully populated by finalize_document
  // before any inline parse, so eager extraction is unnecessary work that
  // also breaks the oneshot=oracle contract by changing when content is
  // dropped from paragraphs. During a tentative partial-line pass the
  // function records its content drop and refmap insertions as txn ops,
  // so the next feed/finish/snapshot reverts both.
  if ((parser->options & CMARK_OPT_FEED_AST))
    try_eager_ref_extract(parser, node);

  cmark_parser_feed_mark_inline_dirty(parser, node);
}

static void remove_trailing_blank_lines(cmark_strbuf *ln) {
  bufsize_t i;
  unsigned char c;

  for (i = ln->size - 1; i >= 0; --i) {
    c = ln->ptr[i];

    if (c != ' ' && c != '\t' && !S_is_line_end_char(c))
      break;
  }

  if (i < 0) {
    cmark_strbuf_clear(ln);
    return;
  }

  for (; i < ln->size; ++i) {
    c = ln->ptr[i];

    if (!S_is_line_end_char(c))
      continue;

    cmark_strbuf_truncate(ln, i);
    break;
  }
}

// Check to see if a node ends with a blank line, descending
// if needed into lists and sublists.
static bool S_ends_with_blank_line(cmark_node *node) {
  if (S_last_line_checked(node)) {
    return(S_last_line_blank(node));
  } else if ((S_type(node) == CMARK_NODE_LIST ||
              S_type(node) == CMARK_NODE_ITEM) && node->last_child) {
    S_set_last_line_checked(node);
    return(S_ends_with_blank_line(node->last_child));
  } else {
    S_set_last_line_checked(node);
    return (S_last_line_blank(node));
  }
}

// returns true if content remains after link defs are resolved.
static bool resolve_reference_link_definitions(
		cmark_parser *parser,
                cmark_node *b) {
  bufsize_t pos;
  cmark_strbuf *node_content = &b->content;
  cmark_chunk chunk = {node_content->ptr, node_content->size, 0};
  bufsize_t prior_content_size = node_content->size;
  while (chunk.len && chunk.data[0] == '[' &&
         (pos = cmark_parse_reference_inline(parser->mem, &chunk,
					     parser->refmap))) {

    chunk.data += pos;
    chunk.len -= pos;
  }
  cmark_strbuf_drop(node_content, (node_content->size - chunk.len));
  if (node_content->size != prior_content_size) {
    // Feed: process_inlines walks the tree unconditionally, but the
    // snapshot path drains the dirty list — without an explicit mark, a
    // paragraph that retained content after ref-def extraction would not
    // be re-inline-parsed against the truncated buffer. No-op when
    // feed mode is inactive.
    cmark_parser_feed_mark_inline_dirty(parser, b);
  }
  return !is_blank(&b->content, 0);
}

// Feed: attempt to extract leading [label]: url ["title"] reference
// definitions from a paragraph's content as soon as we can prove the def is
// bounded — i.e., before the paragraph closes.
//
// Why this is needed: in standard cmark, ref-defs are extracted only when
// their host paragraph finalizes. For incremental input that's a problem:
// a [foo] reference earlier in the document stays as plain text until the
// trailing def-paragraph closes (typically requiring a following blank line
// the user may not have fed yet). Eager extraction lets the def populate
// the refmap as soon as it is unambiguously complete.
//
// Why it must be careful: the title can extend onto the next line (CommonMark
// permits [foo]: url\n   "title"\n). If we extract a def at the moment we
// see [foo]: url\n and the next bytes turn out to be    "title"\n, we
// have committed a different AST than one-shot parse would have produced —
// a contract violation.
//
// Safety rule: a leading def is committable iff the first byte after its
// parsed extent is non-whitespace. Reasoning:
//   - whitespace at that byte could be the leading indent of a title-bearing
//     continuation line; defer until the next byte arrives;
//   - a non-whitespace byte means any title would have had to start on the
//     def line itself (already considered by the parser) and didn't, so the
//     def is final and the trailing content belongs to a subsequent block.
static void try_eager_ref_extract(cmark_parser *parser, cmark_node *p) {
  if (S_type(p) != CMARK_NODE_PARAGRAPH)
    return;
  cmark_strbuf *node_content = &p->content;
  bufsize_t cursor = 0;
  while (cursor < node_content->size && node_content->ptr[cursor] == '[') {
    cmark_chunk dryrun = {node_content->ptr + cursor,
                           node_content->size - cursor, 0};
    bufsize_t parsed_len = cmark_parse_reference_inline(parser->mem, &dryrun,
                                                        /*refmap=*/NULL);
    if (parsed_len == 0)
      break;
    // Safety: can we prove the def is bounded?
    bufsize_t next_byte_offset = cursor + parsed_len;
    if (next_byte_offset >= node_content->size)
      break;  // No bytes after — title might still extend the def.
    unsigned char next_byte = node_content->ptr[next_byte_offset];
    if (next_byte == ' ' || next_byte == '\t')
      break;  // Whitespace lead-in — could be a title-continuation line.
    // Bounded: commit.
    cmark_chunk commit = {node_content->ptr + cursor,
                           node_content->size - cursor, 0};
    cmark_parse_reference_inline(parser->mem, &commit, parser->refmap);
    cursor = next_byte_offset;
  }
  if (cursor > 0) {
    // Feed: log the content state before the drop so the partial-line
    // txn can revert. The cmark_reference_create above already logged
    // REFMAP_ADD for each extracted def; this captures the matching
    // content-buffer mutation. No-op outside an active txn.
    if (cmark_parser_feed_partial_txn_active(parser)) {
      cmark_parser_feed_partial_txn_record_content_rewrite(
          parser, p, node_content->ptr, node_content->size);
    }
    cmark_strbuf_drop(node_content, cursor);
    // Reset inline_parsed_len: it was an offset into the pre-drop content
    // and is meaningless against the shrunken buffer. Leaving it stale
    // would let process_inlines' "already parsed" fast path skip a
    // legitimate reparse if inline_parsed_len happens to match the new
    // content size. Re-attempting from byte 0 in a future call is correct
    // — we always loop from byte 0 of node_content above.
    p->inline_parsed_len = 0;
  }
}

static cmark_node *finalize(cmark_parser *parser, cmark_node *b) {
  bufsize_t pos;
  cmark_node *item;
  cmark_node *subitem;
  cmark_node *parent;
  bool has_content;

  bool tentative = cmark_parser_feed_partial_txn_active(parser);

  // Feed (tentative): snapshot enough state for revert to undo this
  // finalize. CLOSE_BLOCK saves OPEN flag + end position; CONTENT_REWRITE
  // saves the content buffer if finalize will mutate it (paragraph
  // ref-def extraction, code-block content detach, html-block content
  // detach); MORPH (reused with same prior_type) saves the `as` payload
  // for type-specific finalize side effects (code/html literal chunks,
  // list tight flag).
  if (tentative) {
    cmark_parser_feed_partial_txn_record_close_block(parser, b, b->flags,
                                                   b->end_line, b->end_column);
    cmark_node_type t = S_type(b);
    bool will_mutate_content =
        (t == CMARK_NODE_PARAGRAPH ||
         t == CMARK_NODE_CODE_BLOCK ||
         t == CMARK_NODE_HTML_BLOCK);
    if (will_mutate_content && b->content.size > 0) {
      cmark_parser_feed_partial_txn_record_content_rewrite(
          parser, b, b->content.ptr, b->content.size);
    }
    bool will_mutate_as =
        (t == CMARK_NODE_CODE_BLOCK || t == CMARK_NODE_HTML_BLOCK ||
         t == CMARK_NODE_LIST);
    if (will_mutate_as) {
      cmark_parser_feed_partial_txn_record_save_as(parser, b, &b->as,
                                                 sizeof(b->as));
    }
  }

  parent = b->parent;
  assert(b->flags &
         CMARK_NODE__OPEN); // shouldn't call finalize on closed blocks
  b->flags &= ~CMARK_NODE__OPEN;

  if (parser->curline.size == 0) {
    // end of input - line number has not been incremented
    b->end_line = parser->line_number;
    b->end_column = parser->last_line_length;
  } else if (S_type(b) == CMARK_NODE_DOCUMENT ||
             (S_type(b) == CMARK_NODE_CODE_BLOCK && b->as.code.fenced) ||
             (S_type(b) == CMARK_NODE_HEADING && b->as.heading.setext)) {
    b->end_line = parser->line_number;
    b->end_column = parser->curline.size;
    if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\n')
      b->end_column -= 1;
    if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\r')
      b->end_column -= 1;
  } else {
    b->end_line = parser->line_number - 1;
    b->end_column = parser->last_line_length;
  }

  cmark_strbuf *node_content = &b->content;

  switch (S_type(b)) {
  case CMARK_NODE_PARAGRAPH:
  {
    has_content = resolve_reference_link_definitions(parser, b);
    if (!has_content) {
      // Paragraph is only reference defs and should be removed. In
      // tentative mode we unlink (and stash for revert); otherwise free.
      if (tentative) {
        cmark_node *prev = b->prev;
        cmark_node *next = b->next;
        cmark_node *par = b->parent;
        if (par) {
          if (b->prev) b->prev->next = b->next;
          else par->first_child = b->next;
          if (b->next) b->next->prev = b->prev;
          else par->last_child = b->prev;
        }
        b->prev = NULL;
        b->next = NULL;
        b->parent = NULL;
        cmark_parser_feed_partial_txn_record_detach(parser, b, par, prev, next);
      } else {
        cmark_node_free(b);
      }
      return parent;
    }
    break;
  }

  case CMARK_NODE_CODE_BLOCK:
    if (!b->as.code.fenced) { // indented code
      remove_trailing_blank_lines(node_content);
      cmark_strbuf_putc(node_content, '\n');
    } else {
      // first line of contents becomes info
      for (pos = 0; pos < node_content->size; ++pos) {
        if (S_is_line_end_char(node_content->ptr[pos]))
          break;
      }
      assert(pos < node_content->size);

      cmark_strbuf tmp = CMARK_BUF_INIT(parser->mem);
      houdini_unescape_html_f(&tmp, node_content->ptr, pos);
      cmark_strbuf_trim(&tmp);
      cmark_strbuf_unescape(&tmp);
      b->as.code.info = cmark_chunk_buf_detach(&tmp);

      if (node_content->ptr[pos] == '\r')
        pos += 1;
      if (node_content->ptr[pos] == '\n')
        pos += 1;
      cmark_strbuf_drop(node_content, pos);
    }
    b->as.code.literal = cmark_chunk_buf_detach(node_content);
    break;

  case CMARK_NODE_HTML_BLOCK:
    b->as.literal = cmark_chunk_buf_detach(node_content);
    break;

  case CMARK_NODE_LIST:      // determine tight/loose status
    b->as.list.tight = true; // tight by default
    item = b->first_child;

    while (item) {
      // check for non-final non-empty list item ending with blank line:
      if (S_last_line_blank(item) && item->next) {
        b->as.list.tight = false;
        break;
      }
      // recurse into children of list item, to see if there are
      // spaces between them:
      subitem = item->first_child;
      while (subitem) {
        if ((item->next || subitem->next) &&
            S_ends_with_blank_line(subitem)) {
          b->as.list.tight = false;
          break;
        }
        subitem = subitem->next;
      }
      if (!(b->as.list.tight)) {
        break;
      }
      item = item->next;
    }

    break;

  default:
    break;
  }

  return parent;
}

// Add a node as child of another.  Return pointer to child.
static cmark_node *add_child(cmark_parser *parser, cmark_node *parent,
                             cmark_node_type block_type, int start_column) {
  assert(parent);

  // if 'parent' isn't the kind of node that can accept this child,
  // then back up til we hit a node that can.
  while (!cmark_node_can_contain_type(parent, block_type)) {
    parent = finalize(parser, parent);
  }

  cmark_node *child =
      make_block(parser->mem, block_type, parser->line_number, start_column);
  child->parent = parent;

  if (parent->last_child) {
    parent->last_child->next = child;
    child->prev = parent->last_child;
  } else {
    parent->first_child = child;
    child->prev = NULL;
  }
  parent->last_child = child;

  // Feed: log this addition so a partial-line transaction can free the
  // child on revert. No-op outside an active transaction.
  if (cmark_parser_feed_partial_txn_active(parser)) {
    cmark_parser_feed_partial_txn_record_add_child(parser, child);
  }

  return child;
}

void cmark_manage_extensions_special_characters(cmark_parser *parser, int add) {
  cmark_llist *tmp_ext;

  for (tmp_ext = parser->inline_syntax_extensions; tmp_ext; tmp_ext=tmp_ext->next) {
    cmark_syntax_extension *ext = (cmark_syntax_extension *) tmp_ext->data;
    cmark_llist *tmp_char;
    for (tmp_char = ext->special_inline_chars; tmp_char; tmp_char=tmp_char->next) {
      unsigned char c = (unsigned char)(size_t)tmp_char->data;
      if (add)
        cmark_inlines_add_special_character(c, ext->emphasis);
      else
        cmark_inlines_remove_special_character(c, ext->emphasis);
    }
  }
}

// Walk through node and all children, recursively, parsing
// string content into inline content where appropriate.
//
// Feed: blocks whose inline content has already been parsed by
// cmark_parser_snapshot — and not modified since — are skipped. The
// indicator is `inline_parsed_len == content.size && !INLINE_DIRTY`. New
// content arriving via add_line resets INLINE_DIRTY, so this skip only
// fires for genuinely up-to-date subtrees.
static void process_inlines(cmark_parser *parser,
                            cmark_map *refmap, int options) {
  cmark_iter *iter = cmark_iter_new(parser->root);
  cmark_node *cur;
  cmark_event_type ev_type;

  cmark_manage_extensions_special_characters(parser, true);

  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cur = cmark_iter_get_node(iter);
    if (ev_type == CMARK_EVENT_ENTER) {
      if (cmark_block_contains_inlines(cur)) {
        bool already_parsed =
            !(cur->flags & CMARK_NODE__INLINE_DIRTY) &&
            cur->inline_parsed_len == cur->content.size &&
            cur->content.size > 0;
        if (!already_parsed) {
          // Feed: cmark_iter_next yielded ENTER cur and immediately
          // cached iter->next.node = cur->first_child. We're about to free
          // those children, so reset the iterator to skip past cur — its
          // freshly-rebuilt children are inlines, which contains_inlines is
          // never true for, so we'd not act on them anyway.
          while (cur->first_child) {
            cmark_node_free(cur->first_child);
          }
          cmark_parse_inlines(parser, cur, refmap, options);
          cur->inline_parsed_len = cur->content.size;
          cur->flags &= ~CMARK_NODE__INLINE_DIRTY;
          cmark_iter_reset(iter, cur, CMARK_EVENT_EXIT);
        }
      }
    }
  }

  cmark_manage_extensions_special_characters(parser, false);

  cmark_iter_free(iter);
}

static int sort_footnote_by_ix(const void *_a, const void *_b) {
  cmark_footnote *a = *(cmark_footnote **)_a;
  cmark_footnote *b = *(cmark_footnote **)_b;
  return (int)a->ix - (int)b->ix;
}

static void process_footnotes(cmark_parser *parser) {
  // * Collect definitions in a map.
  // * Iterate the references in the document in order, assigning indices to
  //   definitions in the order they're seen.
  // * Write out the footnotes at the bottom of the document in index order.

  cmark_map *map = cmark_footnote_map_new(parser->mem);

  cmark_iter *iter = cmark_iter_new(parser->root);
  cmark_node *cur;
  cmark_event_type ev_type;

  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cur = cmark_iter_get_node(iter);
    if (ev_type == CMARK_EVENT_EXIT && cur->type == CMARK_NODE_FOOTNOTE_DEFINITION) {
      cmark_footnote_create(map, cur);
    }
  }

  cmark_iter_free(iter);
  iter = cmark_iter_new(parser->root);
  unsigned int ix = 0;

  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cur = cmark_iter_get_node(iter);
    if (ev_type == CMARK_EVENT_EXIT && cur->type == CMARK_NODE_FOOTNOTE_REFERENCE) {
      cmark_footnote *footnote = (cmark_footnote *)cmark_map_lookup(map, &cur->as.literal);
      if (footnote) {
        if (!footnote->ix)
          footnote->ix = ++ix;

        // store a reference to this footnote reference's footnote definition
        // this is used by renderers when generating label ids
        cur->parent_footnote_def = footnote->node;

        // keep track of a) count of how many times this footnote def has been
        // referenced, and b) which reference index this footnote ref is at.
        // this is used by renderers when generating links and backreferences.
        cur->footnote.ref_ix = ++footnote->node->footnote.def_count;

        char n[32];
        snprintf(n, sizeof(n), "%d", footnote->ix);
        cmark_chunk_free(parser->mem, &cur->as.literal);
        cmark_strbuf buf = CMARK_BUF_INIT(parser->mem);
        cmark_strbuf_puts(&buf, n);

        cur->as.literal = cmark_chunk_buf_detach(&buf);
      } else {
        cmark_node *text = (cmark_node *)parser->mem->calloc(1, sizeof(*text));
        cmark_strbuf_init(parser->mem, &text->content, 0);
        text->type = (uint16_t) CMARK_NODE_TEXT;

        cmark_strbuf buf = CMARK_BUF_INIT(parser->mem);
        cmark_strbuf_puts(&buf, "[^");
        cmark_strbuf_put(&buf, cur->as.literal.data, cur->as.literal.len);
        cmark_strbuf_putc(&buf, ']');

        text->as.literal = cmark_chunk_buf_detach(&buf);
        cmark_node_insert_after(cur, text);
        cmark_node_free(cur);
      }
    }
  }

  cmark_iter_free(iter);

  if (map->sorted) {
    qsort(map->sorted, map->size, sizeof(cmark_map_entry *), sort_footnote_by_ix);
    for (unsigned int i = 0; i < map->size; ++i) {
      cmark_footnote *footnote = (cmark_footnote *)map->sorted[i];
      if (!footnote->ix) {
        cmark_node_unlink(footnote->node);
        continue;
      }
      cmark_node_append_child(parser->root, footnote->node);
      footnote->node = NULL;
    }
  }

  cmark_unlink_footnotes_map(map);
  cmark_map_free(map);
}

// Attempts to parse a list item marker (bullet or enumerated).
// On success, returns length of the marker, and populates
// data with the details.  On failure, returns 0.
static bufsize_t parse_list_marker(cmark_mem *mem, cmark_chunk *input,
                                   bufsize_t pos, bool interrupts_paragraph,
                                   cmark_list **dataptr) {
  unsigned char c;
  bufsize_t startpos;
  cmark_list *data;
  bufsize_t i;

  startpos = pos;
  c = peek_at(input, pos);

  if (c == '*' || c == '-' || c == '+') {
    pos++;
    if (!cmark_isspace(peek_at(input, pos))) {
      return 0;
    }

    if (interrupts_paragraph) {
      i = pos;
      // require non-blank content after list marker:
      while (S_is_space_or_tab(peek_at(input, i))) {
        i++;
      }
      if (peek_at(input, i) == '\n') {
        return 0;
      }
    }

    data = (cmark_list *)mem->calloc(1, sizeof(*data));
    data->marker_offset = 0; // will be adjusted later
    data->list_type = CMARK_BULLET_LIST;
    data->bullet_char = c;
    data->start = 0;
    data->delimiter = CMARK_NO_DELIM;
    data->tight = false;
  } else if (cmark_isdigit(c)) {
    int start = 0;
    int digits = 0;

    do {
      start = (10 * start) + (peek_at(input, pos) - '0');
      pos++;
      digits++;
      // We limit to 9 digits to avoid overflow,
      // assuming max int is 2^31 - 1
      // This also seems to be the limit for 'start' in some browsers.
    } while (digits < 9 && cmark_isdigit(peek_at(input, pos)));

    if (interrupts_paragraph && start != 1) {
      return 0;
    }
    c = peek_at(input, pos);
    if (c == '.' || c == ')') {
      pos++;
      if (!cmark_isspace(peek_at(input, pos))) {
        return 0;
      }
      if (interrupts_paragraph) {
        // require non-blank content after list marker:
        i = pos;
        while (S_is_space_or_tab(peek_at(input, i))) {
          i++;
        }
        if (S_is_line_end_char(peek_at(input, i))) {
          return 0;
        }
      }

      data = (cmark_list *)mem->calloc(1, sizeof(*data));
      data->marker_offset = 0; // will be adjusted later
      data->list_type = CMARK_ORDERED_LIST;
      data->bullet_char = 0;
      data->start = start;
      data->delimiter = (c == '.' ? CMARK_PERIOD_DELIM : CMARK_PAREN_DELIM);
      data->tight = false;
    } else {
      return 0;
    }
  } else {
    return 0;
  }

  *dataptr = data;
  return (pos - startpos);
}

// Return 1 if list item belongs in list, else 0.
static int lists_match(cmark_list *list_data, cmark_list *item_data) {
  return (list_data->list_type == item_data->list_type &&
          list_data->delimiter == item_data->delimiter &&
          // list_data->marker_offset == item_data.marker_offset &&
          list_data->bullet_char == item_data->bullet_char);
}

static cmark_node *finalize_document(cmark_parser *parser) {
  while (parser->current != parser->root) {
    parser->current = finalize(parser, parser->current);
  }

  finalize(parser, parser->root);

  // Limit total size of extra content created from reference links to
  // document size to avoid superlinear growth. Always allow 100KB.
  if (parser->total_size > 100000)
    parser->refmap->max_ref_size = parser->total_size;
  else
    parser->refmap->max_ref_size = 100000;

  process_inlines(parser, parser->refmap, parser->options);
  if (parser->options & CMARK_OPT_FOOTNOTES)
    process_footnotes(parser);

  return parser->root;
}

cmark_node *cmark_parse_file(FILE *f, int options) {
  unsigned char buffer[4096];
  cmark_parser *parser = cmark_parser_new(options);
  size_t bytes;
  cmark_node *document;

  while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    bool eof = bytes < sizeof(buffer);
    S_parser_feed(parser, buffer, bytes, eof);
    if (eof) {
      break;
    }
  }

  document = cmark_parser_finish(parser);
  cmark_parser_free(parser);
  return document;
}

cmark_node *cmark_parse_document(const char *buffer, size_t len, int options) {
  cmark_parser *parser = cmark_parser_new(options);
  cmark_node *document;

  if (options & CMARK_OPT_FEED_AST) {
    // Feed opt-in: route through the unified feed + finish pipeline.
    // cmark_parser_new already activated feed bookkeeping, so feed
    // takes the feed-aware path; finish reverts any tentative state
    // and runs canonical finalize. The final tree must be byte-identical
    // to the pristine path below — that equivalence is what makes the
    // pristine path a usable oracle.
    cmark_parser_feed(parser, buffer, len);
  } else {
    // Pristine oneshot path: does not touch the feed active-parser
    // TLS or feed bookkeeping. cmark_parser_snapshot is never called
    // on this parser, feed_active stays false, and every
    // feed-aware hook short-circuits — preserving the contract that
    // an out-of-the-box cmark_parse_document call is the behavioral
    // oracle for feed output.
    S_parser_feed(parser, (const unsigned char *)buffer, len, true);
  }

  document = cmark_parser_finish(parser);
  cmark_parser_free(parser);
  return document;
}

void cmark_parser_feed(cmark_parser *parser, const char *buffer, size_t len) {
  // Pristine path until feed-mode has been activated by a snapshot call:
  // skip the active-parser TLS dance and tentative-revert entirely. Once
  // feed mode is active, mutation primitives need to find this parser via
  // TLS so they can record txn ops / drive pending-ref resolution, and any
  // previously-open tentative txn must be reverted before new canonical
  // bytes append to the linebuf.
  if (!(parser->options & CMARK_OPT_FEED_AST)) {
    S_parser_feed(parser, (const unsigned char *)buffer, len, false);
    return;
  }

  cmark_parser *prev = cmark_parser_feed_active_parser();
  cmark_parser_feed_set_active_parser(parser);
  if (cmark_parser_feed_partial_txn_active(parser))
    cmark_parser_feed_partial_txn_revert(parser);
  S_parser_feed(parser, (const unsigned char *)buffer, len, false);
  cmark_parser_feed_set_active_parser(prev);
}

void cmark_parser_feed_process_partial_line(cmark_parser *parser) {
  // Caller (cmark_parser_snapshot) owns the partial-line txn lifecycle so
  // both this step and the subsequent tentative finalize can record into a
  // single txn. We only run the line through S_process_line; the active
  // txn captures whatever mutations result.
  if (!cmark_parser_feed_partial_txn_active(parser))
    return;
  if (parser->linebuf.size == 0)
    return;

  // Build a NUL-terminated working copy so we can hand bytes to
  // S_process_line without disturbing parser->linebuf. Append a synthetic
  // newline — S_process_line normalizes by appending '\n' itself if the
  // input lacks one, but we make the intent explicit here.
  cmark_strbuf tmp = CMARK_BUF_INIT(parser->mem);
  cmark_strbuf_put(&tmp, parser->linebuf.ptr, parser->linebuf.size);
  cmark_strbuf_putc(&tmp, '\n');
  S_process_line(parser, tmp.ptr, tmp.size);
  cmark_strbuf_free(&tmp);

  // Run any inline parses queued by the tentative line. The inline-parse
  // op records the prior children chain so revert can restore them.
  cmark_parser_feed_run_pending_inlines(parser);
}

void cmark_parser_feed_tentative_finalize(cmark_parser *parser) {
  // Tentatively close every open block so the snapshot tree mirrors what
  // cmark_parse_document(prefix) would produce after EOF. finalize() routes
  // through the active partial-line txn (CLOSE_BLOCK + CONTENT_REWRITE +
  // SAVE_AS + DETACH + REFMAP_ADD), so revert restores the canonical
  // mid-stream state on the next feed/finish/snapshot.
  if (!cmark_parser_feed_partial_txn_active(parser))
    return;
  while (parser->current != parser->root) {
    parser->current = finalize(parser, parser->current);
  }
  finalize(parser, parser->root);
}

void cmark_parser_feed_reentrant(cmark_parser *parser, const char *buffer, size_t len) {
  cmark_strbuf saved_linebuf;

  // Pristine path: same gating as cmark_parser_feed. If feed mode has
  // never been activated, no txn can exist and no mutation primitive needs
  // to discover the parser via TLS.
  if (!(parser->options & CMARK_OPT_FEED_AST)) {
    cmark_strbuf_init(parser->mem, &saved_linebuf, 0);
    cmark_strbuf_puts(&saved_linebuf, cmark_strbuf_cstr(&parser->linebuf));
    cmark_strbuf_clear(&parser->linebuf);

    size_t saved_total_size = parser->total_size;
    S_parser_feed(parser, (const unsigned char *)buffer, len, true);
    parser->total_size = saved_total_size;

    cmark_strbuf_sets(&parser->linebuf, cmark_strbuf_cstr(&saved_linebuf));
    cmark_strbuf_free(&saved_linebuf);
    return;
  }

  // Feed-active path: commit any pending partial-line txn before
  // injecting bytes. Without this, S_parser_feed below runs against the
  // tentative tree and its mutations land in the still-open txn — which
  // the next normal feed/finish/snapshot then reverts, silently dropping
  // the injection.
  if (cmark_parser_feed_partial_txn_active(parser))
    cmark_parser_feed_partial_txn_revert(parser);

  cmark_strbuf_init(parser->mem, &saved_linebuf, 0);
  cmark_strbuf_puts(&saved_linebuf, cmark_strbuf_cstr(&parser->linebuf));
  cmark_strbuf_clear(&parser->linebuf);

  // Save total_size so the injected text doesn't pollute the outer parse's
  // view of input length (used to size refmap memory limits).
  size_t saved_total_size = parser->total_size;

  cmark_parser *prev = cmark_parser_feed_active_parser();
  cmark_parser_feed_set_active_parser(parser);
  S_parser_feed(parser, (const unsigned char *)buffer, len, true);
  cmark_parser_feed_set_active_parser(prev);

  parser->total_size = saved_total_size;

  cmark_strbuf_sets(&parser->linebuf, cmark_strbuf_cstr(&saved_linebuf));
  cmark_strbuf_free(&saved_linebuf);
}

static void S_parser_feed(cmark_parser *parser, const unsigned char *buffer,
                          size_t len, bool eof) {
  const unsigned char *end = buffer + len;
  static const uint8_t repl[] = {239, 191, 189};

  if (len > UINT_MAX - parser->total_size)
    parser->total_size = UINT_MAX;
  else
    parser->total_size += len;

  if (parser->last_buffer_ended_with_cr && *buffer == '\n') {
    // skip NL if last buffer ended with CR ; see #117
    buffer++;
  }
  parser->last_buffer_ended_with_cr = false;
  while (buffer < end) {
    const unsigned char *eol;
    bufsize_t chunk_len;
    bool process = false;
    for (eol = buffer; eol < end; ++eol) {
      if (S_is_line_end_char(*eol)) {
        process = true;
        break;
      }
      if (*eol == '\0' && eol < end) {
        break;
      }
    }
    if (eol >= end && eof) {
      process = true;
    }

    chunk_len = (bufsize_t)(eol - buffer);
    if (process) {
      if (parser->linebuf.size > 0) {
        cmark_strbuf_put(&parser->linebuf, buffer, chunk_len);
        S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
        cmark_strbuf_clear(&parser->linebuf);
      } else {
        S_process_line(parser, buffer, chunk_len);
      }
    } else {
      if (eol < end && *eol == '\0') {
        // omit NULL byte
        cmark_strbuf_put(&parser->linebuf, buffer, chunk_len);
        // add replacement character
        cmark_strbuf_put(&parser->linebuf, repl, 3);
      } else {
        cmark_strbuf_put(&parser->linebuf, buffer, chunk_len);
      }
    }

    buffer += chunk_len;
    if (buffer < end) {
      if (*buffer == '\0') {
        // skip over NULL
        buffer++;
      } else {
        // skip over line ending characters
        if (*buffer == '\r') {
          buffer++;
          if (buffer == end)
            parser->last_buffer_ended_with_cr = true;
        }
        if (buffer < end && *buffer == '\n') {
          buffer++;
        }
      }
    }
  }
}

static void chop_trailing_hashtags(cmark_chunk *ch) {
  bufsize_t n, orig_n;

  cmark_chunk_rtrim(ch);
  orig_n = n = ch->len - 1;

  // if string ends in space followed by #s, remove these:
  while (n >= 0 && peek_at(ch, n) == '#')
    n--;

  // Check for a space before the final #s:
  if (n != orig_n && n >= 0 && S_is_space_or_tab(peek_at(ch, n))) {
    ch->len = n;
    cmark_chunk_rtrim(ch);
  }
}

// Check for thematic break.  On failure, return 0 and update
// thematic_break_kill_pos with the index at which the
// parse fails.  On success, return length of match.
// "...three or more hyphens, asterisks,
// or underscores on a line by themselves. If you wish, you may use
// spaces between the hyphens or asterisks."
static int S_scan_thematic_break(cmark_parser *parser, cmark_chunk *input,
                                 bufsize_t offset) {
  bufsize_t i;
  char c;
  char nextc = '\0';
  int count;
  i = offset;
  c = peek_at(input, i);
  if (!(c == '*' || c == '_' || c == '-')) {
    parser->thematic_break_kill_pos = i;
    return 0;
  }
  count = 1;
  while ((nextc = peek_at(input, ++i))) {
    if (nextc == c) {
      count++;
    } else if (nextc != ' ' && nextc != '\t') {
      break;
    }
  }
  if (count >= 3 && (nextc == '\r' || nextc == '\n')) {
    return (i - offset) + 1;
  } else {
    parser->thematic_break_kill_pos = i;
    return 0;
  }
}

// Find first nonspace character from current offset, setting
// parser->first_nonspace, parser->first_nonspace_column,
// parser->indent, and parser->blank. Does not advance parser->offset.
static void S_find_first_nonspace(cmark_parser *parser, cmark_chunk *input) {
  char c;
  int chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);

  if (parser->first_nonspace <= parser->offset) {
    parser->first_nonspace = parser->offset;
    parser->first_nonspace_column = parser->column;
    while ((c = peek_at(input, parser->first_nonspace))) {
      if (c == ' ') {
        parser->first_nonspace += 1;
        parser->first_nonspace_column += 1;
        chars_to_tab = chars_to_tab - 1;
        if (chars_to_tab == 0) {
          chars_to_tab = TAB_STOP;
        }
      } else if (c == '\t') {
        parser->first_nonspace += 1;
        parser->first_nonspace_column += chars_to_tab;
        chars_to_tab = TAB_STOP;
      } else {
        break;
      }
    }
  }

  parser->indent = parser->first_nonspace_column - parser->column;
  parser->blank = S_is_line_end_char(peek_at(input, parser->first_nonspace));
}

// Advance parser->offset and parser->column.  parser->offset is the
// byte position in input; parser->column is a virtual column number
// that takes into account tabs. (Multibyte characters are not taken
// into account, because the Markdown line prefixes we are interested in
// analyzing are entirely ASCII.)  The count parameter indicates
// how far to advance the offset.  If columns is true, then count
// indicates a number of columns; otherwise, a number of bytes.
// If advancing a certain number of columns partially consumes
// a tab character, parser->partially_consumed_tab is set to true.
static void S_advance_offset(cmark_parser *parser, cmark_chunk *input,
                             bufsize_t count, bool columns) {
  char c;
  int chars_to_tab;
  int chars_to_advance;
  while (count > 0 && (c = peek_at(input, parser->offset))) {
    if (c == '\t') {
      chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
      if (columns) {
        parser->partially_consumed_tab = chars_to_tab > count;
        chars_to_advance = MIN(count, chars_to_tab);
        parser->column += chars_to_advance;
        parser->offset += (parser->partially_consumed_tab ? 0 : 1);
        count -= chars_to_advance;
      } else {
        parser->partially_consumed_tab = false;
        parser->column += chars_to_tab;
        parser->offset += 1;
        count -= 1;
      }
    } else {
      parser->partially_consumed_tab = false;
      parser->offset += 1;
      parser->column += 1; // assume ascii; block starts are ascii
      count -= 1;
    }
  }
}

static bool S_last_child_is_open(cmark_node *container) {
  return container->last_child &&
         (container->last_child->flags & CMARK_NODE__OPEN);
}

static bool parse_block_quote_prefix(cmark_parser *parser, cmark_chunk *input) {
  bool res = false;
  bufsize_t matched = 0;

  matched =
      parser->indent <= 3 && peek_at(input, parser->first_nonspace) == '>';
  if (matched) {

    S_advance_offset(parser, input, parser->indent + 1, true);

    if (S_is_space_or_tab(peek_at(input, parser->offset))) {
      S_advance_offset(parser, input, 1, true);
    }

    res = true;
  }
  return res;
}

static bool parse_footnote_definition_block_prefix(cmark_parser *parser, cmark_chunk *input,
                                                   cmark_node *container) {
  if (parser->indent >= 4) {
    S_advance_offset(parser, input, 4, true);
    return true;
  } else if (input->len > 0 && (input->data[0] == '\n' || (input->data[0] == '\r' && input->data[1] == '\n'))) {
    return true;
  }

  return false;
}

static bool parse_node_item_prefix(cmark_parser *parser, cmark_chunk *input,
                                   cmark_node *container) {
  bool res = false;

  if (parser->indent >=
      container->as.list.marker_offset + container->as.list.padding) {
    S_advance_offset(parser, input, container->as.list.marker_offset +
                                        container->as.list.padding,
                     true);
    res = true;
  } else if (parser->blank && container->first_child != NULL) {
    // if container->first_child is NULL, then the opening line
    // of the list item was blank after the list marker; in this
    // case, we are done with the list item.
    S_advance_offset(parser, input, parser->first_nonspace - parser->offset,
                     false);
    res = true;
  }
  return res;
}

static bool parse_code_block_prefix(cmark_parser *parser, cmark_chunk *input,
                                    cmark_node *container,
                                    bool *should_continue) {
  bool res = false;

  if (!container->as.code.fenced) { // indented
    if (parser->indent >= CODE_INDENT) {
      S_advance_offset(parser, input, CODE_INDENT, true);
      res = true;
    } else if (parser->blank) {
      S_advance_offset(parser, input, parser->first_nonspace - parser->offset,
                       false);
      res = true;
    }
  } else { // fenced
    bufsize_t matched = 0;

    if (parser->indent <= 3 && (peek_at(input, parser->first_nonspace) ==
                                container->as.code.fence_char)) {
      matched = scan_close_code_fence(input, parser->first_nonspace);
    }

    if (matched >= container->as.code.fence_length) {
      // closing fence - and since we're at
      // the end of a line, we can stop processing it:
      *should_continue = false;
      S_advance_offset(parser, input, matched, false);
      parser->current = finalize(parser, container);
    } else {
      // skip opt. spaces of fence parser->offset
      int i = container->as.code.fence_offset;

      while (i > 0 && S_is_space_or_tab(peek_at(input, parser->offset))) {
        S_advance_offset(parser, input, 1, true);
        i--;
      }
      res = true;
    }
  }

  return res;
}

static bool parse_html_block_prefix(cmark_parser *parser,
                                    cmark_node *container) {
  bool res = false;
  int html_block_type = container->as.html_block_type;

  assert(html_block_type >= 1 && html_block_type <= 7);
  switch (html_block_type) {
  case 1:
  case 2:
  case 3:
  case 4:
  case 5:
    // these types of blocks can accept blanks
    res = true;
    break;
  case 6:
  case 7:
    res = !parser->blank;
    break;
  }

  return res;
}

static bool parse_extension_block(cmark_parser *parser,
                                  cmark_node *container,
                                  cmark_chunk *input)
{
  bool res = false;

  if (container->extension->last_block_matches) {
    if (container->extension->last_block_matches(
        container->extension, parser, input->data, input->len, container))
      res = true;
  }

  return res;
}

/**
 * For each containing node, try to parse the associated line start.
 *
 * Will not close unmatched blocks, as we may have a lazy continuation
 * line -> http://spec.commonmark.org/0.24/#lazy-continuation-line
 *
 * Returns: The last matching node, or NULL
 */
static cmark_node *check_open_blocks(cmark_parser *parser, cmark_chunk *input,
                                     bool *all_matched) {
  bool should_continue = true;
  *all_matched = false;
  cmark_node *container = parser->root;
  cmark_node_type cont_type;

  while (S_last_child_is_open(container)) {
    container = container->last_child;
    cont_type = S_type(container);

    S_find_first_nonspace(parser, input);

    if (container->extension) {
      if (!parse_extension_block(parser, container, input))
        goto done;
      continue;
    }

    switch (cont_type) {
    case CMARK_NODE_BLOCK_QUOTE:
      if (!parse_block_quote_prefix(parser, input))
        goto done;
      break;
    case CMARK_NODE_ITEM:
      if (!parse_node_item_prefix(parser, input, container))
        goto done;
      break;
    case CMARK_NODE_CODE_BLOCK:
      if (!parse_code_block_prefix(parser, input, container, &should_continue))
        goto done;
      break;
    case CMARK_NODE_HEADING:
      // a heading can never contain more than one line
      goto done;
    case CMARK_NODE_HTML_BLOCK:
      if (!parse_html_block_prefix(parser, container))
        goto done;
      break;
    case CMARK_NODE_PARAGRAPH:
      if (parser->blank)
        goto done;
      break;
		case CMARK_NODE_FOOTNOTE_DEFINITION:
			if (!parse_footnote_definition_block_prefix(parser, input, container))
				goto done;
			break;
    default:
      break;
    }
  }

  *all_matched = true;

done:
  if (!*all_matched) {
    container = container->parent; // back up to last matching node
  }

  if (!should_continue) {
    container = NULL;
  }

  return container;
}

static void open_new_blocks(cmark_parser *parser, cmark_node **container,
                            cmark_chunk *input, bool all_matched) {
  bool indented;
  cmark_list *data = NULL;
  bool maybe_lazy = S_type(parser->current) == CMARK_NODE_PARAGRAPH;
  cmark_node_type cont_type = S_type(*container);
  bufsize_t matched = 0;
  int lev = 0;
  bool save_partially_consumed_tab;
  bool has_content;
  int save_offset;
  int save_column;
  size_t depth = 0;

  while (cont_type != CMARK_NODE_CODE_BLOCK &&
         cont_type != CMARK_NODE_HTML_BLOCK) {
    depth++;
    S_find_first_nonspace(parser, input);
    indented = parser->indent >= CODE_INDENT;

    if (!indented && peek_at(input, parser->first_nonspace) == '>') {

      bufsize_t blockquote_startpos = parser->first_nonspace;

      S_advance_offset(parser, input,
                       parser->first_nonspace + 1 - parser->offset, false);
      // optional following character
      if (S_is_space_or_tab(peek_at(input, parser->offset))) {
        S_advance_offset(parser, input, 1, true);
      }
      *container = add_child(parser, *container, CMARK_NODE_BLOCK_QUOTE,
                             blockquote_startpos + 1);

    } else if (!indented && (matched = scan_atx_heading_start(
                                 input, parser->first_nonspace))) {
      bufsize_t hashpos;
      int level = 0;
      bufsize_t heading_startpos = parser->first_nonspace;

      S_advance_offset(parser, input,
                       parser->first_nonspace + matched - parser->offset,
                       false);
      *container = add_child(parser, *container, CMARK_NODE_HEADING,
                             heading_startpos + 1);

      hashpos = cmark_chunk_strchr(input, '#', parser->first_nonspace);

      while (peek_at(input, hashpos) == '#') {
        level++;
        hashpos++;
      }

      (*container)->as.heading.level = level;
      (*container)->as.heading.setext = false;
      (*container)->internal_offset = matched;

    } else if (!indented && (matched = scan_open_code_fence(
                                 input, parser->first_nonspace))) {
      *container = add_child(parser, *container, CMARK_NODE_CODE_BLOCK,
                             parser->first_nonspace + 1);
      (*container)->as.code.fenced = true;
      (*container)->as.code.fence_char = peek_at(input, parser->first_nonspace);
      (*container)->as.code.fence_length = (matched > 255) ? 255 : (uint8_t)matched;
      (*container)->as.code.fence_offset =
          (int8_t)(parser->first_nonspace - parser->offset);
      (*container)->as.code.info = cmark_chunk_literal("");
      S_advance_offset(parser, input,
                       parser->first_nonspace + matched - parser->offset,
                       false);

    } else if (!indented && ((matched = scan_html_block_start(
                                  input, parser->first_nonspace)) ||
                             (cont_type != CMARK_NODE_PARAGRAPH &&
                              (matched = scan_html_block_start_7(
                                   input, parser->first_nonspace))))) {
      *container = add_child(parser, *container, CMARK_NODE_HTML_BLOCK,
                             parser->first_nonspace + 1);
      (*container)->as.html_block_type = matched;
      // note, we don't adjust parser->offset because the tag is part of the
      // text
    } else if (!indented && cont_type == CMARK_NODE_PARAGRAPH &&
               (lev =
                    scan_setext_heading_line(input, parser->first_nonspace))) {
      // finalize paragraph, resolving reference links
      has_content = resolve_reference_link_definitions(parser, *container);

      if (has_content) {

        // Rewrite the paragraph in place. Going through cmark_node_set_type
        // (rather than a direct type assignment) routes the rewrite through
        // the feed event stream: pointer identity is preserved, and a
        // RETYPED record is emitted for any active snapshot consumer.
        cmark_node_set_type(*container, CMARK_NODE_HEADING);
        (*container)->as.heading.level = lev;
        (*container)->as.heading.setext = true;
        S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
      }
    } else if (!indented &&
               !(cont_type == CMARK_NODE_PARAGRAPH && !all_matched) &&
	       (parser->thematic_break_kill_pos <= parser->first_nonspace) &&
               (matched = S_scan_thematic_break(parser, input, parser->first_nonspace))) {
      // it's only now that we know the line is not part of a setext heading:
      *container = add_child(parser, *container, CMARK_NODE_THEMATIC_BREAK,
                             parser->first_nonspace + 1);
      S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
    } else if (!indented &&
               (parser->options & CMARK_OPT_FOOTNOTES) &&
               depth < MAX_LIST_DEPTH &&
               (matched = scan_footnote_definition(input, parser->first_nonspace))) {
      cmark_chunk c = cmark_chunk_dup(input, parser->first_nonspace + 2, matched - 2);

      while (c.data[c.len - 1] != ']')
        --c.len;
      --c.len;

      cmark_chunk_to_cstr(parser->mem, &c);

      S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
      *container = add_child(parser, *container, CMARK_NODE_FOOTNOTE_DEFINITION, parser->first_nonspace + matched + 1);
      (*container)->as.literal = c;

      (*container)->internal_offset = matched;
    } else if ((!indented || cont_type == CMARK_NODE_LIST) &&
	       parser->indent < 4 &&
               depth < MAX_LIST_DEPTH &&
               (matched = parse_list_marker(
                    parser->mem, input, parser->first_nonspace,
                    (*container)->type == CMARK_NODE_PARAGRAPH, &data))) {

      // Note that we can have new list items starting with >= 4
      // spaces indent, as long as the list container is still open.
      int i = 0;

      // compute padding:
      S_advance_offset(parser, input,
                       parser->first_nonspace + matched - parser->offset,
                       false);

      save_partially_consumed_tab = parser->partially_consumed_tab;
      save_offset = parser->offset;
      save_column = parser->column;

      while (parser->column - save_column <= 5 &&
             S_is_space_or_tab(peek_at(input, parser->offset))) {
        S_advance_offset(parser, input, 1, true);
      }

      i = parser->column - save_column;
      if (i >= 5 || i < 1 ||
          // only spaces after list marker:
          S_is_line_end_char(peek_at(input, parser->offset))) {
        data->padding = matched + 1;
        parser->offset = save_offset;
        parser->column = save_column;
        parser->partially_consumed_tab = save_partially_consumed_tab;
        if (i > 0) {
          S_advance_offset(parser, input, 1, true);
        }
      } else {
        data->padding = matched + i;
      }

      // check container; if it's a list, see if this list item
      // can continue the list; otherwise, create a list container.

      data->marker_offset = parser->indent;

      if (cont_type != CMARK_NODE_LIST ||
          !lists_match(&((*container)->as.list), data)) {
        *container = add_child(parser, *container, CMARK_NODE_LIST,
                               parser->first_nonspace + 1);

        memcpy(&((*container)->as.list), data, sizeof(*data));
      }

      // add the list item
      *container = add_child(parser, *container, CMARK_NODE_ITEM,
                             parser->first_nonspace + 1);
      /* TODO: static */
      memcpy(&((*container)->as.list), data, sizeof(*data));
      parser->mem->free(data);
    } else if (indented && !maybe_lazy && !parser->blank) {
      S_advance_offset(parser, input, CODE_INDENT, true);
      *container = add_child(parser, *container, CMARK_NODE_CODE_BLOCK,
                             parser->offset + 1);
      (*container)->as.code.fenced = false;
      (*container)->as.code.fence_char = 0;
      (*container)->as.code.fence_length = 0;
      (*container)->as.code.fence_offset = 0;
      (*container)->as.code.info = cmark_chunk_literal("");
    } else {
      cmark_llist *tmp;
      cmark_node *new_container = NULL;

      for (tmp = parser->syntax_extensions; tmp; tmp=tmp->next) {
        cmark_syntax_extension *ext = (cmark_syntax_extension *) tmp->data;

        if (ext->try_opening_block) {
          new_container = ext->try_opening_block(
              ext, indented, parser, *container, input->data, input->len);

          if (new_container) {
            *container = new_container;
            break;
          }
        }
      }

      if (!new_container) {
        break;
      }
    }

    if (accepts_lines(S_type(*container))) {
      // if it's a line container, it can't contain other containers
      break;
    }

    cont_type = S_type(*container);
    maybe_lazy = false;
  }
}

static void add_text_to_container(cmark_parser *parser, cmark_node *container,
                                  cmark_node *last_matched_container,
                                  cmark_chunk *input) {
  cmark_node *tmp;
  // what remains at parser->offset is a text line.  add the text to the
  // appropriate container.

  S_find_first_nonspace(parser, input);

  if (parser->blank && container->last_child)
    S_set_last_line_blank(container->last_child, true);

  // block quote lines are never blank as they start with >
  // and we don't count blanks in fenced code for purposes of tight/loose
  // lists or breaking out of lists.  we also don't set last_line_blank
  // on an empty list item.
  const cmark_node_type ctype = S_type(container);
  const bool last_line_blank =
      (parser->blank && ctype != CMARK_NODE_BLOCK_QUOTE &&
       ctype != CMARK_NODE_HEADING && ctype != CMARK_NODE_THEMATIC_BREAK &&
       !(ctype == CMARK_NODE_CODE_BLOCK && container->as.code.fenced) &&
       !(ctype == CMARK_NODE_ITEM && container->first_child == NULL &&
         container->start_line == parser->line_number));

  S_set_last_line_blank(container, last_line_blank);

  tmp = container;
  while (tmp->parent) {
    S_set_last_line_blank(tmp->parent, false);
    tmp = tmp->parent;
  }

  // If the last line processed belonged to a paragraph node,
  // and we didn't match all of the line prefixes for the open containers,
  // and we didn't start any new containers,
  // and the line isn't blank,
  // then treat this as a "lazy continuation line" and add it to
  // the open paragraph.
  if (parser->current != last_matched_container &&
      container == last_matched_container && !parser->blank &&
      S_type(parser->current) == CMARK_NODE_PARAGRAPH) {
    add_line(parser->current, input, parser);
  } else { // not a lazy continuation
    // Finalize any blocks that were not matched and set cur to container:
    while (parser->current != last_matched_container) {
      parser->current = finalize(parser, parser->current);
      assert(parser->current != NULL);
    }

    if (S_type(container) == CMARK_NODE_CODE_BLOCK) {
      add_line(container, input, parser);
    } else if (S_type(container) == CMARK_NODE_HTML_BLOCK) {
      add_line(container, input, parser);

      int matches_end_condition;
      switch (container->as.html_block_type) {
      case 1:
        // </script>, </style>, </pre>
        matches_end_condition =
            scan_html_block_end_1(input, parser->first_nonspace);
        break;
      case 2:
        // -->
        matches_end_condition =
            scan_html_block_end_2(input, parser->first_nonspace);
        break;
      case 3:
        // ?>
        matches_end_condition =
            scan_html_block_end_3(input, parser->first_nonspace);
        break;
      case 4:
        // >
        matches_end_condition =
            scan_html_block_end_4(input, parser->first_nonspace);
        break;
      case 5:
        // ]]>
        matches_end_condition =
            scan_html_block_end_5(input, parser->first_nonspace);
        break;
      default:
        matches_end_condition = 0;
        break;
      }

      if (matches_end_condition) {
        container = finalize(parser, container);
        assert(parser->current != NULL);
      }
    } else if (parser->blank) {
      // ??? do nothing
    } else if (accepts_lines(S_type(container))) {
      if (S_type(container) == CMARK_NODE_HEADING &&
          container->as.heading.setext == false) {
        chop_trailing_hashtags(input);
      }
      S_advance_offset(parser, input, parser->first_nonspace - parser->offset,
                       false);
      add_line(container, input, parser);
    } else {
      // create paragraph container for line
      container = add_child(parser, container, CMARK_NODE_PARAGRAPH,
                            parser->first_nonspace + 1);
      S_advance_offset(parser, input, parser->first_nonspace - parser->offset,
                       false);
      add_line(container, input, parser);
    }

    parser->current = container;
  }
}

/* See http://spec.commonmark.org/0.24/#phase-1-block-structure */
static void S_process_line(cmark_parser *parser, const unsigned char *buffer,
                           bufsize_t bytes) {
  cmark_node *last_matched_container;
  bool all_matched = true;
  cmark_node *container;
  cmark_chunk input;
  cmark_node *current;

  cmark_strbuf_clear(&parser->curline);

  if (parser->options & CMARK_OPT_VALIDATE_UTF8)
    cmark_utf8proc_check(&parser->curline, buffer, bytes);
  else
    cmark_strbuf_put(&parser->curline, buffer, bytes);

  bytes = parser->curline.size;

  // ensure line ends with a newline:
  if (bytes == 0 || !S_is_line_end_char(parser->curline.ptr[bytes - 1]))
    cmark_strbuf_putc(&parser->curline, '\n');

  parser->offset = 0;
  parser->column = 0;
  parser->first_nonspace = 0;
  parser->first_nonspace_column = 0;
  parser->thematic_break_kill_pos = 0;
  parser->indent = 0;
  parser->blank = false;
  parser->partially_consumed_tab = false;

  input.data = parser->curline.ptr;
  input.len = parser->curline.size;
  input.alloc = 0;

  // Skip UTF-8 BOM.
  if (parser->line_number == 0 &&
      input.len >= 3 &&
      memcmp(input.data, "\xef\xbb\xbf", 3) == 0)
    parser->offset += 3;

  parser->line_number++;

  last_matched_container = check_open_blocks(parser, &input, &all_matched);

  if (!last_matched_container)
    goto finished;

  container = last_matched_container;

  current = parser->current;

  open_new_blocks(parser, &container, &input, all_matched);

  /* parser->current might have changed if feed_reentrant was called */
  if (current == parser->current)
  add_text_to_container(parser, container, last_matched_container, &input);

finished:
  parser->last_line_length = input.len;
  if (parser->last_line_length &&
      input.data[parser->last_line_length - 1] == '\n')
    parser->last_line_length -= 1;
  if (parser->last_line_length &&
      input.data[parser->last_line_length - 1] == '\r')
    parser->last_line_length -= 1;

  cmark_strbuf_clear(&parser->curline);
}

void cmark_parser_extension_postprocess(cmark_parser *parser) {
  cmark_llist *exts;
  for (exts = parser->syntax_extensions; exts; exts = exts->next) {
    cmark_syntax_extension *ext = (cmark_syntax_extension *)exts->data;
    if (ext->postprocess_func) {
      cmark_node *processed = ext->postprocess_func(ext, parser, parser->root);
      if (processed)
        parser->root = processed;
    }
  }
}

cmark_node *cmark_parser_finish(cmark_parser *parser) {
  cmark_node *res;
  cmark_parser *prev_active = NULL;
  bool feed_active = (parser->options & CMARK_OPT_FEED_AST);

  /* Parser was already finished once */
  if (parser->root == NULL)
    return NULL;

  // Only stream-active parsers need the active-parser TLS dance and the
  // tentative-revert. A pristine feed+finish without snapshot stays on the
  // oneshot path with no feed hooks visible to it.
  if (feed_active) {
    prev_active = cmark_parser_feed_active_parser();
    cmark_parser_feed_set_active_parser(parser);
    if (cmark_parser_feed_partial_txn_active(parser))
      cmark_parser_feed_partial_txn_revert(parser);
  }

  if (parser->linebuf.size) {
    S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
    cmark_strbuf_clear(&parser->linebuf);
  }

  finalize_document(parser);

  // Text-node consolidation is now part of cmark_parse_inlines' contract;
  // every block reachable through finalize_document → process_inlines is
  // already merged on return. No tree-wide post-pass needed.

  cmark_strbuf_free(&parser->curline);
  cmark_strbuf_free(&parser->linebuf);

#if CMARK_DEBUG_NODES
  if (cmark_node_check(parser->root, stderr)) {
    abort();
  }
#endif

  cmark_parser_extension_postprocess(parser);

  res = parser->root;
  parser->root = NULL;

  cmark_parser_reset(parser);

  if (feed_active)
    cmark_parser_feed_set_active_parser(prev_active);

  return res;
}

int cmark_parser_get_line_number(cmark_parser *parser) {
  return parser->line_number;
}

bufsize_t cmark_parser_get_offset(cmark_parser *parser) {
  return parser->offset;
}

bufsize_t cmark_parser_get_column(cmark_parser *parser) {
  return parser->column;
}

int cmark_parser_get_first_nonspace(cmark_parser *parser) {
  return parser->first_nonspace;
}

int cmark_parser_get_first_nonspace_column(cmark_parser *parser) {
  return parser->first_nonspace_column;
}

int cmark_parser_get_indent(cmark_parser *parser) {
  return parser->indent;
}

int cmark_parser_is_blank(cmark_parser *parser) {
  return parser->blank;
}

int cmark_parser_has_partially_consumed_tab(cmark_parser *parser) {
  return parser->partially_consumed_tab;
}

int cmark_parser_get_last_line_length(cmark_parser *parser) {
  return parser->last_line_length;
}

cmark_node *cmark_parser_add_child(cmark_parser *parser,
                                   cmark_node   *parent,
                                   cmark_node_type block_type,
                                   int start_column) {
  return add_child(parser, parent, block_type, start_column);
}

void cmark_parser_advance_offset(cmark_parser *parser,
                                 const char *input,
                                 int count,
                                 int columns) {
  cmark_chunk input_chunk = cmark_chunk_literal(input);

  S_advance_offset(parser, &input_chunk, count, columns != 0);
}

void cmark_parser_set_backslash_ispunct_func(cmark_parser *parser,
                                             cmark_ispunct_func func) {
  parser->backslash_ispunct = func;
}

cmark_llist *cmark_parser_get_syntax_extensions(cmark_parser *parser) {
  return parser->syntax_extensions;
}

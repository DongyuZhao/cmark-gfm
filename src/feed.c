// Feed-driven AST support for cmark-gfm.
//
// Provides incremental snapshots of the parse tree while feed() is in
// progress. The convergence contract is: after cmark_parser_feed(B_k) and
// cmark_parser_snapshot(), the returned tree is structurally equivalent to
// cmark_parse_document(B_k, ...). To meet this for byte prefixes that end
// mid-line, snapshot tentatively runs the standard S_process_line on
// (linebuf + '\n') so any block-level disambiguation (setext promotion,
// table morph, ATX heading start, fenced code, ref-def) surfaces in the
// returned tree. Every mutation S_process_line performs is logged in a
// per-snapshot transaction; the next call to feed/finish/snapshot reverts
// it so the canonical state continues incrementally from where it was.

#include <stdlib.h>
#include <string.h>

#include "cmark-gfm.h"
#include "cmark_ctype.h"
#include "inlines.h"
#include "map.h"
#include "node.h"
#include "parser.h"
#include "feed.h"
#include "syntax_extension.h"

// Thread-local active-parser pointer. Lets node mutation primitives (which
// have public, parser-less signatures) record txn entries when running
// inside cmark_parser_feed / cmark_parser_finish / cmark_parser_snapshot.
// Falls back to plain static if no thread-local is available.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    !defined(__STDC_NO_THREADS__)
  static _Thread_local cmark_parser *s_active_parser;
#elif defined(__GNUC__) || defined(__clang__)
  static __thread cmark_parser *s_active_parser;
#else
  static cmark_parser *s_active_parser;
#endif

void cmark_parser_feed_set_active_parser(cmark_parser *p) {
  s_active_parser = p;
}

cmark_parser *cmark_parser_feed_active_parser(void) {
  return s_active_parser;
}

void cmark_parser_feed_state_init(struct cmark_parser_feed_state *s) {
  memset(s, 0, sizeof(*s));
}

static void free_pending_refs(cmark_mem *mem,
                              struct cmark_pending_ref_user *head) {
  while (head) {
    struct cmark_pending_ref_user *next = head->next;
    cmark_chunk_free(mem, &head->label);
    if (head->normalized_label)
      mem->free(head->normalized_label);
    mem->free(head);
    head = next;
  }
}

// Free a chain linked by ->next (used for detached / morph-stashed nodes
// that the txn was holding when it is destroyed without revert).
static void free_node_chain(cmark_node *head) {
  while (head) {
    cmark_node *next = head->next;
    head->next = NULL;
    head->prev = NULL;
    head->parent = NULL;
    cmark_node_free(head);
    head = next;
  }
}

static void free_partial_txn(cmark_mem *mem,
                              struct cmark_partial_line_txn *t) {
  if (!t)
    return;
  for (size_t i = 0; i < t->records_len; ++i) {
    struct cmark_partial_line_txn_record *r = &t->records[i];
    if (r->op == CMARK_TXN_OP_MORPH && r->detached_children_first) {
      // Children that were not re-linked by revert (e.g. revert never ran).
      free_node_chain(r->detached_children_first);
    }
    if (r->op == CMARK_TXN_OP_DETACH_NODE && r->node) {
      // The detached node is alive and orphaned; free it.
      cmark_node_free(r->node);
    }
    if (r->op == CMARK_TXN_OP_INLINE_PARSE && r->prior_inline_first) {
      free_node_chain(r->prior_inline_first);
    }
    if (r->op == CMARK_TXN_OP_CONTENT_REWRITE && r->prior_content_bytes) {
      mem->free(r->prior_content_bytes);
    }
  }
  if (t->records)
    mem->free(t->records);
  if (t->flag_snapshot)
    mem->free(t->flag_snapshot);
  mem->free(t);
}

void cmark_parser_feed_state_free(cmark_mem *mem,
                                struct cmark_parser_feed_state *s) {
  free_pending_refs(mem, s->pending_ref_users_head);
  free_partial_txn(mem, s->partial_txn);
  // The dirty-blocks list is intrusive into the node tree; the nodes
  // themselves are owned by the document tree and freed elsewhere.
  memset(s, 0, sizeof(*s));
}

// Detach (without freeing) the inline-children chain of `block`. Returns the
// chain head; sets *out_last to the chain tail. The chain pointers (next/prev)
// remain wired internally; only block->first_child / block->last_child are
// cleared. Caller becomes responsible for the chain's lifetime.
static cmark_node *detach_block_children(cmark_node *block,
                                          cmark_node **out_last) {
  cmark_node *head = block->first_child;
  cmark_node *tail = block->last_child;
  for (cmark_node *c = head; c; c = c->next) {
    c->parent = NULL;
  }
  block->first_child = NULL;
  block->last_child = NULL;
  if (out_last)
    *out_last = tail;
  return head;
}

static void free_block_children(cmark_node *block) {
  while (block->first_child) {
    cmark_node_free(block->first_child);
  }
}

// Re-attach a previously-detached chain as the children of `block`. The
// caller has just built fresh children that should now be replaced by the
// stashed chain.
static void reattach_block_children(cmark_node *block,
                                     cmark_node *first,
                                     cmark_node *last) {
  // Free whatever children were placed on `block` after the detach.
  free_block_children(block);
  block->first_child = first;
  block->last_child = last;
  for (cmark_node *c = first; c; c = c->next) {
    c->parent = block;
  }
}

// Inline-parse a single dirty block (or emit no-op for non-inline-bearing
// leaves). Caller manages extension specials around a batch and unlinks
// from the dirty list before calling.
static void process_one_dirty_block(cmark_parser *parser, cmark_node *block) {
  block->dirty_next = NULL;
  block->flags &= ~CMARK_NODE__INLINE_DIRTY;

  if (block->parent == NULL && block != parser->root)
    return;  // detached — nothing to parse.

  if (!cmark_block_contains_inlines(block)) {
    block->inline_parsed_len = block->content.size;
    return;
  }

  bool tentative = cmark_parser_feed_partial_txn_active(parser);

  // Incremental append-only resume — canonical (non-tentative) path only.
  // Preconditions, all required:
  //   1. Not inside a tentative txn. Tentative parses must remain
  //      destructive so revert can restore the pre-tentative children
  //      chain via INLINE_PARSE op; resume would leave prior children
  //      interleaved with new ones with no clean undo.
  //   2. Prior parse left both delimiter and bracket stacks empty
  //      (CLEAN_END flag set) — empty stacks at EOF mean no `*`/`_`/`[`
  //      was left dangling that a future byte could close, so emitted
  //      children are stable.
  //   3. Content only grew past inline_parsed_len. Shrinks (eager
  //      ref-extract drop, content rewrite) reset inline_parsed_len to 0
  //      and clear CLEAN_END, so this reduces to a simple range check.
  //   4. inline_parsed_len > 0. A first parse on a non-empty block has
  //      no prior children to preserve, so resume vs from-scratch are
  //      equivalent — take the simpler path.
  if (!tentative &&
      (block->flags & CMARK_NODE__INLINE_CLEAN_END) &&
      block->inline_parsed_len > 0 &&
      block->content.size > block->inline_parsed_len) {
    // Compute the actual resume cursor. cmark_parse_inlines rtrims trailing
    // whitespace from subj.input before parsing, which means the prior
    // parse stopped at the rtrim'd-length of the prior content — not at
    // inline_parsed_len. Concretely: prior content "Hello\n" had
    // inline_parsed_len=6 but the parser only emitted children for bytes
    // [0,5); byte 5 (the trailing '\n') was rtrim'd off and produced no
    // softbreak. When new content "Hello\n world\n" arrives, resume must
    // start at byte 5 so the now-non-trailing '\n' becomes a softbreak
    // between "Hello" and " world". Walking backward through trailing
    // whitespace of the prior-content slice gives us that anchor; bytes
    // [resume_offset, content.size) contain everything the new parse
    // needs to emit, including any newly-non-trailing whitespace.
    bufsize_t resume_offset = block->inline_parsed_len;
    while (resume_offset > 0 &&
           cmark_isspace((char)block->content.ptr[resume_offset - 1])) {
      resume_offset--;
    }
    cmark_parse_inlines_resume(parser, block, parser->refmap, parser->options,
                               resume_offset);
    block->inline_parsed_len = block->content.size;
    return;
  }

  // Full reparse path.
  if (tentative && block->first_child != NULL) {
    cmark_node *prior_last = NULL;
    cmark_node *prior_first = detach_block_children(block, &prior_last);
    cmark_parser_feed_partial_txn_record_inline_parse(parser, block,
                                                    prior_first, prior_last,
                                                    block->inline_parsed_len);
  } else if (tentative) {
    // No prior children but we still need to record so revert can clear
    // the inlines we are about to parse.
    cmark_parser_feed_partial_txn_record_inline_parse(parser, block,
                                                    NULL, NULL,
                                                    block->inline_parsed_len);
  } else {
    free_block_children(block);
  }

  cmark_parse_inlines(parser, block, parser->refmap, parser->options);
  block->inline_parsed_len = block->content.size;
}

void cmark_parser_feed_run_pending_inlines(cmark_parser *parser) {
  cmark_node *block = cmark_parser_feed_drain_dirty_blocks(parser);
  if (!block)
    return;
  cmark_manage_extensions_special_characters(parser, true);
  while (block) {
    cmark_node *next = block->dirty_next;
    process_one_dirty_block(parser, block);
    block = next;
  }
  cmark_manage_extensions_special_characters(parser, false);
}

void cmark_parser_feed_mark_inline_dirty(cmark_parser *parser, cmark_node *block) {
  if (!block)
    return;
  if (!(parser->options & CMARK_OPT_FEED_AST))
    return;
  if (block->flags & CMARK_NODE__INLINE_DIRTY)
    return;
  block->flags |= CMARK_NODE__INLINE_DIRTY;
  block->dirty_next = parser->feed.dirty_blocks_head;
  parser->feed.dirty_blocks_head = block;
}

cmark_node *cmark_parser_feed_drain_dirty_blocks(cmark_parser *parser) {
  cmark_node *head = parser->feed.dirty_blocks_head;
  parser->feed.dirty_blocks_head = NULL;
  return head;
}

// ------------------------------------------------------------------
// Partial-line transaction
// ------------------------------------------------------------------

// Walk the document tree and snapshot every block's flags into the txn.
// Called by partial_txn_begin so revert can restore extension memoization
// bits (e.g. CMARK_NODE__TABLE_VISITED) that the tentative pass might set.
//
// Skip inline nodes: snapshotted flags (OPEN, LAST_LINE_*, INLINE_DIRTY,
// REGISTER_FIRST) and extension are block-level concerns, and ADD_LINE /
// INLINE_PARSE reverts may free already-parsed inline children — recording
// them would leave dangling snapshot entries that the revert loop then
// writes through.
static void capture_flag_snapshot(cmark_parser *parser,
                                   struct cmark_partial_line_txn *t) {
  if (!parser->root)
    return;
  cmark_iter *iter = cmark_iter_new(parser->root);
  cmark_event_type ev;
  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    if (ev != CMARK_EVENT_ENTER)
      continue;
    cmark_node *cur = cmark_iter_get_node(iter);
    if (!CMARK_NODE_BLOCK_P(cur))
      continue;
    if (t->flag_snapshot_len == t->flag_snapshot_cap) {
      size_t new_cap = t->flag_snapshot_cap ? t->flag_snapshot_cap * 2 : 16;
      struct cmark_node_flag_snapshot *resized =
          (struct cmark_node_flag_snapshot *)parser->mem->realloc(
              t->flag_snapshot,
              new_cap * sizeof(struct cmark_node_flag_snapshot));
      t->flag_snapshot = resized;
      t->flag_snapshot_cap = new_cap;
    }
    t->flag_snapshot[t->flag_snapshot_len].node = cur;
    t->flag_snapshot[t->flag_snapshot_len].flags = cur->flags;
    t->flag_snapshot[t->flag_snapshot_len].extension = cur->extension;
    t->flag_snapshot_len++;
  }
  cmark_iter_free(iter);
}

void cmark_parser_feed_partial_txn_begin(cmark_parser *parser) {
  struct cmark_parser_feed_state *s = &parser->feed;
  if (s->partial_txn) {
    // Prior txn should have been reverted — defensive cleanup.
    free_partial_txn(parser->mem, s->partial_txn);
    s->partial_txn = NULL;
  }
  struct cmark_partial_line_txn *t =
      (struct cmark_partial_line_txn *)parser->mem->calloc(
          1, sizeof(struct cmark_partial_line_txn));
  t->prior_current = parser->current;
  t->prior_line_number = parser->line_number;
  t->prior_last_line_length = parser->last_line_length;
  t->prior_blank = parser->blank;
  t->prior_partially_consumed_tab = parser->partially_consumed_tab;
  t->prior_offset = parser->offset;
  t->prior_column = parser->column;
  t->prior_first_nonspace = parser->first_nonspace;
  t->prior_first_nonspace_column = parser->first_nonspace_column;
  t->prior_thematic_break_kill_pos = parser->thematic_break_kill_pos;
  t->prior_indent = parser->indent;
  t->prior_total_size = parser->total_size;
  t->prior_last_buffer_ended_with_cr = parser->last_buffer_ended_with_cr;
  s->partial_txn = t;
  capture_flag_snapshot(parser, t);
}

bool cmark_parser_feed_partial_txn_active(cmark_parser *parser) {
  return parser && parser->feed.partial_txn != NULL;
}

void cmark_parser_feed_partial_txn_mark_unrevertable(cmark_parser *parser) {
  if (parser && parser->feed.partial_txn)
    parser->feed.partial_txn->unrevertable = true;
}

static struct cmark_partial_line_txn_record *
txn_alloc_record(cmark_parser *parser) {
  struct cmark_partial_line_txn *t = parser->feed.partial_txn;
  if (!t)
    return NULL;
  if (t->records_len == t->records_cap) {
    size_t new_cap = t->records_cap ? t->records_cap * 2 : 8;
    struct cmark_partial_line_txn_record *resized =
        (struct cmark_partial_line_txn_record *)parser->mem->realloc(
            t->records,
            new_cap * sizeof(struct cmark_partial_line_txn_record));
    t->records = resized;
    t->records_cap = new_cap;
  }
  struct cmark_partial_line_txn_record *r = &t->records[t->records_len++];
  memset(r, 0, sizeof(*r));
  return r;
}

void cmark_parser_feed_partial_txn_record_add_child(cmark_parser *parser,
                                                  cmark_node *child) {
  struct cmark_partial_line_txn_record *r = txn_alloc_record(parser);
  if (!r)
    return;
  r->op = CMARK_TXN_OP_ADD_CHILD;
  r->node = child;
}

void cmark_parser_feed_partial_txn_record_add_line(cmark_parser *parser,
                                                 cmark_node *node,
                                                 bufsize_t prior_size) {
  struct cmark_partial_line_txn_record *r = txn_alloc_record(parser);
  if (!r)
    return;
  r->op = CMARK_TXN_OP_ADD_LINE;
  r->node = node;
  r->prior_content_size = prior_size;
  r->prior_inline_parsed_len = node->inline_parsed_len;
}

void cmark_parser_feed_partial_txn_record_save_as(cmark_parser *parser,
                                                cmark_node *node,
                                                const void *prior_as_bytes,
                                                size_t prior_as_size) {
  struct cmark_partial_line_txn_record *r = txn_alloc_record(parser);
  if (!r)
    return;
  r->op = CMARK_TXN_OP_SAVE_AS;
  r->node = node;
  if (prior_as_size > sizeof(r->prior_as_bytes)) {
    cmark_parser_feed_partial_txn_mark_unrevertable(parser);
    return;
  }
  memcpy(r->prior_as_bytes, prior_as_bytes, prior_as_size);
  r->prior_as_present = true;
}

void cmark_parser_feed_partial_txn_record_morph(cmark_parser *parser,
                                              cmark_node *node,
                                              uint16_t prior_type,
                                              const void *prior_as_bytes,
                                              size_t prior_as_size,
                                              cmark_node *detached_first,
                                              cmark_node *detached_last) {
  struct cmark_partial_line_txn_record *r = txn_alloc_record(parser);
  if (!r) {
    // Safety: free the detached chain if we can't record (allocation
    // failure path); revert wouldn't see them otherwise.
    free_node_chain(detached_first);
    return;
  }
  r->op = CMARK_TXN_OP_MORPH;
  r->node = node;
  r->prior_type = prior_type;
  if (prior_as_size > sizeof(r->prior_as_bytes)) {
    // Should never happen — the static array is sized for the largest
    // sizeof(node->as). Bail gracefully by marking unrevertable.
    cmark_parser_feed_partial_txn_mark_unrevertable(parser);
    free_node_chain(detached_first);
    return;
  }
  memcpy(r->prior_as_bytes, prior_as_bytes, prior_as_size);
  r->prior_as_present = true;
  r->detached_children_first = detached_first;
  r->detached_children_last = detached_last;
}

void cmark_parser_feed_partial_txn_record_close_block(
    cmark_parser *parser, cmark_node *node,
    cmark_node_internal_flags prior_flags, int prior_end_line,
    int prior_end_column) {
  struct cmark_partial_line_txn_record *r = txn_alloc_record(parser);
  if (!r)
    return;
  r->op = CMARK_TXN_OP_CLOSE_BLOCK;
  r->node = node;
  r->prior_flags = prior_flags;
  r->prior_end_line = prior_end_line;
  r->prior_end_column = prior_end_column;
}

void cmark_parser_feed_partial_txn_record_refmap_add(
    cmark_parser *parser, struct cmark_map_entry *entry) {
  struct cmark_partial_line_txn_record *r = txn_alloc_record(parser);
  if (!r)
    return;
  r->op = CMARK_TXN_OP_REFMAP_ADD;
  r->added_entry = entry;
}

void cmark_parser_feed_partial_txn_record_detach(cmark_parser *parser,
                                               cmark_node *node,
                                               cmark_node *parent,
                                               cmark_node *prev,
                                               cmark_node *next) {
  struct cmark_partial_line_txn_record *r = txn_alloc_record(parser);
  if (!r) {
    // Allocation failure — free the orphaned node since revert won't run.
    if (node)
      cmark_node_free(node);
    return;
  }
  r->op = CMARK_TXN_OP_DETACH_NODE;
  r->node = node;
  r->detach_parent = parent;
  r->detach_prev = prev;
  r->detach_next = next;
}

void cmark_parser_feed_partial_txn_record_inline_parse(
    cmark_parser *parser, cmark_node *block, cmark_node *prior_first,
    cmark_node *prior_last, bufsize_t prior_parsed_len) {
  struct cmark_partial_line_txn_record *r = txn_alloc_record(parser);
  if (!r) {
    free_node_chain(prior_first);
    return;
  }
  r->op = CMARK_TXN_OP_INLINE_PARSE;
  r->node = block;
  r->prior_inline_first = prior_first;
  r->prior_inline_last = prior_last;
  r->prior_inline_parsed_len = prior_parsed_len;
}

void cmark_parser_feed_partial_txn_record_content_rewrite(
    cmark_parser *parser, cmark_node *node, const unsigned char *prior_bytes,
    bufsize_t prior_len) {
  struct cmark_partial_line_txn_record *r = txn_alloc_record(parser);
  if (!r)
    return;
  r->op = CMARK_TXN_OP_CONTENT_REWRITE;
  r->node = node;
  r->prior_content_bytes_len = prior_len;
  if (prior_len > 0) {
    r->prior_content_bytes =
        (unsigned char *)parser->mem->calloc(1, (size_t)prior_len);
    memcpy(r->prior_content_bytes, prior_bytes, (size_t)prior_len);
  }
}

// Forward declaration of free_node_as from node.c (file-private there). We
// re-implement a minimal version here since revert needs it.
//
// NOTE: this does NOT touch node->as.opaque. Opaque ownership depends on
// the op being reverted: MORPH allocated a fresh opaque for the new type
// and must drop it before memcpy clobbers the slot, while SAVE_AS leaves
// opaque untouched (prior_as.opaque == current as.opaque, so freeing here
// would yield a UAF after the memcpy restore). Each revert site handles
// its own opaque lifetime.
static void feed_free_as(cmark_node *node) {
  cmark_mem *mem = node->content.mem;
  switch (node->type) {
    case CMARK_NODE_CODE_BLOCK:
      cmark_chunk_free(mem, &node->as.code.info);
      cmark_chunk_free(mem, &node->as.code.literal);
      break;
    case CMARK_NODE_TEXT:
    case CMARK_NODE_HTML_INLINE:
    case CMARK_NODE_CODE:
    case CMARK_NODE_HTML_BLOCK:
    case CMARK_NODE_FOOTNOTE_REFERENCE:
    case CMARK_NODE_FOOTNOTE_DEFINITION:
      cmark_chunk_free(mem, &node->as.literal);
      break;
    case CMARK_NODE_LINK:
    case CMARK_NODE_IMAGE:
      cmark_chunk_free(mem, &node->as.link.url);
      cmark_chunk_free(mem, &node->as.link.title);
      break;
    case CMARK_NODE_CUSTOM_BLOCK:
    case CMARK_NODE_CUSTOM_INLINE:
      cmark_chunk_free(mem, &node->as.custom.on_enter);
      cmark_chunk_free(mem, &node->as.custom.on_exit);
      break;
    default:
      break;
  }
}

// Revert a single op. Returns true on success.
static bool revert_op(cmark_parser *parser,
                       struct cmark_partial_line_txn_record *r) {
  switch (r->op) {
  case CMARK_TXN_OP_ADD_CHILD: {
    if (r->node)
      cmark_node_free(r->node);
    r->node = NULL;
    return true;
  }
  case CMARK_TXN_OP_ADD_LINE: {
    cmark_node *n = r->node;
    if (!n)
      return true;
    cmark_strbuf_truncate(&n->content, r->prior_content_size);
    n->inline_parsed_len = 0;
    while (n->first_child)
      cmark_node_free(n->first_child);
    if (!(n->flags & CMARK_NODE__INLINE_DIRTY))
      cmark_parser_feed_mark_inline_dirty(parser, n);
    return true;
  }
  case CMARK_TXN_OP_MORPH: {
    cmark_node *n = r->node;
    if (!n || !r->prior_as_present)
      return false;
    // Free current type's `as` resources (e.g. heading no-op, code-block
    // would free chunks). Extension opaque was allocated fresh for the
    // post-morph type (e.g. GFM table allocates node_table + alignments)
    // and must be freed before memcpy clobbers the pointer with the
    // prior type's `as` bytes — otherwise the allocation leaks.
    if (n->as.opaque && n->extension && n->extension->opaque_free_func)
      n->extension->opaque_free_func(n->extension, n->content.mem, n);
    feed_free_as(n);
    n->type = r->prior_type;
    memcpy(&n->as, r->prior_as_bytes, sizeof(n->as));
    // Free any children the post-morph caller added (e.g. table extension
    // building rows after the type change). Re-attach the stashed pre-morph
    // chain — empty if the node had no children before morph.
    free_block_children(n);
    if (r->detached_children_first) {
      n->first_child = r->detached_children_first;
      n->last_child = r->detached_children_last;
      for (cmark_node *c = r->detached_children_first; c; c = c->next)
        c->parent = n;
      r->detached_children_first = NULL;
      r->detached_children_last = NULL;
    }
    return true;
  }
  case CMARK_TXN_OP_CLOSE_BLOCK: {
    cmark_node *n = r->node;
    if (!n)
      return true;
    n->flags = r->prior_flags;
    n->end_line = r->prior_end_line;
    n->end_column = r->prior_end_column;
    return true;
  }
  case CMARK_TXN_OP_REFMAP_ADD: {
    if (!r->added_entry)
      return true;
    cmark_map *map = parser->refmap;
    cmark_map_entry **slot = &map->refs;
    while (*slot && *slot != r->added_entry)
      slot = &(*slot)->next;
    if (*slot == r->added_entry) {
      *slot = r->added_entry->next;
      map->free(map, r->added_entry);
      // sorted cache must be regenerated; cmark_reference_create already
      // invalidated it on insert, but a subsequent lookup might have
      // re-sorted. Invalidate again.
      if (map->sorted) {
        map->mem->free(map->sorted);
        map->sorted = NULL;
      }
    }
    r->added_entry = NULL;
    return true;
  }
  case CMARK_TXN_OP_DETACH_NODE: {
    cmark_node *n = r->node;
    cmark_node *parent = r->detach_parent;
    if (!n || !parent)
      return false;
    // Re-link at original position.
    n->parent = parent;
    n->prev = r->detach_prev;
    n->next = r->detach_next;
    if (r->detach_prev)
      r->detach_prev->next = n;
    else
      parent->first_child = n;
    if (r->detach_next)
      r->detach_next->prev = n;
    else
      parent->last_child = n;
    r->node = NULL;  // node now lives in the tree again.
    return true;
  }
  case CMARK_TXN_OP_INLINE_PARSE: {
    cmark_node *n = r->node;
    if (!n)
      return true;
    if (r->prior_inline_first) {
      reattach_block_children(n, r->prior_inline_first, r->prior_inline_last);
      r->prior_inline_first = NULL;
      r->prior_inline_last = NULL;
    } else {
      // No prior children — just clear current ones.
      free_block_children(n);
    }
    n->inline_parsed_len = r->prior_inline_parsed_len;
    if (!(n->flags & CMARK_NODE__INLINE_DIRTY))
      cmark_parser_feed_mark_inline_dirty(parser, n);
    return true;
  }
  case CMARK_TXN_OP_CONTENT_REWRITE: {
    cmark_node *n = r->node;
    if (!n)
      return true;
    cmark_strbuf_clear(&n->content);
    if (r->prior_content_bytes && r->prior_content_bytes_len > 0) {
      cmark_strbuf_put(&n->content, r->prior_content_bytes,
                       r->prior_content_bytes_len);
    }
    parser->mem->free(r->prior_content_bytes);
    r->prior_content_bytes = NULL;
    r->prior_content_bytes_len = 0;
    return true;
  }
  case CMARK_TXN_OP_SAVE_AS: {
    cmark_node *n = r->node;
    if (!n || !r->prior_as_present)
      return false;
    feed_free_as(n);
    memcpy(&n->as, r->prior_as_bytes, sizeof(n->as));
    return true;
  }
  }
  return false;
}

void cmark_parser_feed_partial_txn_revert(cmark_parser *parser) {
  struct cmark_parser_feed_state *s = &parser->feed;
  struct cmark_partial_line_txn *t = s->partial_txn;
  if (!t)
    return;
  // Walk records in reverse: each op's revert undoes the most recent
  // mutation first, restoring earlier state in order.
  for (size_t i = t->records_len; i-- > 0;) {
    revert_op(parser, &t->records[i]);
  }
  // Restore the pre-tentative flag bits on every node that existed before
  // tentative ran. Nodes added during tentative were already freed by the
  // ADD_CHILD reverts above; nodes detached but stashed (DETACH/MORPH
  // chains) are re-linked first by those revert ops, then fall under this
  // restore.
  //
  // INLINE_DIRTY is preserved across the restore: revert ops above
  // (ADD_LINE, INLINE_PARSE) intentionally re-mark blocks dirty so the
  // next snapshot reparses them, AND they push the block onto
  // dirty_blocks_head. Letting flag_snapshot blindly clear INLINE_DIRTY
  // here would desync the flag from the list — the block stays in
  // dirty_blocks_head with no flag, and the next mark_inline_dirty call
  // (which short-circuits on the flag) instead re-pushes the block,
  // creating a `dirty_next == self` self-loop. If the block is later
  // freed as a ref-def-only paragraph, cmark_parser_feed_forget_node skips the
  // dirty-list cleanup (also flag-gated) and the next snapshot walks
  // freed memory.
  for (size_t i = 0; i < t->flag_snapshot_len; ++i) {
    struct cmark_node_flag_snapshot *snap = &t->flag_snapshot[i];
    if (snap->node) {
      cmark_node_internal_flags preserved_dirty =
          snap->node->flags & CMARK_NODE__INLINE_DIRTY;
      snap->node->flags = snap->flags | preserved_dirty;
      // Restore extension: a tentative table morph sets node->extension on
      // the paragraph (via cmark_node_set_syntax_extension) and the
      // canonical pass after revert needs it cleared back to its pre-
      // tentative value or the extension's matchers misroute.
      snap->node->extension = snap->extension;
    }
  }
  // Restore parser scalar state.
  parser->current = t->prior_current;
  parser->line_number = t->prior_line_number;
  parser->last_line_length = t->prior_last_line_length;
  parser->blank = t->prior_blank;
  parser->partially_consumed_tab = t->prior_partially_consumed_tab;
  parser->offset = t->prior_offset;
  parser->column = t->prior_column;
  parser->first_nonspace = t->prior_first_nonspace;
  parser->first_nonspace_column = t->prior_first_nonspace_column;
  parser->thematic_break_kill_pos = t->prior_thematic_break_kill_pos;
  parser->indent = t->prior_indent;
  parser->total_size = t->prior_total_size;
  parser->last_buffer_ended_with_cr = t->prior_last_buffer_ended_with_cr;

  free_partial_txn(parser->mem, t);
  s->partial_txn = NULL;
  // Tentative is gone; any future snapshot must rebuild from scratch.
  s->tentative_clean = false;
}

// ------------------------------------------------------------------
// Pending-ref index
// ------------------------------------------------------------------

void cmark_parser_feed_forget_node(cmark_parser *parser, cmark_node *node) {
  if (!parser || !node)
    return;
  struct cmark_parser_feed_state *s = &parser->feed;

  // Active partial-line txn: scrub references to the node about to be freed.
  if (s->partial_txn) {
    struct cmark_partial_line_txn *t = s->partial_txn;
    if (t->prior_current == node)
      t->prior_current = node->parent;
    for (size_t i = 0; i < t->records_len; ++i) {
      struct cmark_partial_line_txn_record *r = &t->records[i];
      if (r->node == node)
        r->node = NULL;
      if (r->detach_parent == node)
        r->detach_parent = NULL;
      if (r->detach_prev == node)
        r->detach_prev = NULL;
      if (r->detach_next == node)
        r->detach_next = NULL;
    }
    for (size_t i = 0; i < t->flag_snapshot_len; ++i) {
      if (t->flag_snapshot[i].node == node)
        t->flag_snapshot[i].node = NULL;
    }
  }

  if (node->flags & CMARK_NODE__INLINE_DIRTY) {
    cmark_node **slot = &s->dirty_blocks_head;
    while (*slot) {
      if (*slot == node) {
        *slot = node->dirty_next;
        node->dirty_next = NULL;
        break;
      }
      slot = &(*slot)->dirty_next;
    }
    node->flags &= ~CMARK_NODE__INLINE_DIRTY;
  }

  // Drop any pending [ref] users registered against this node.
  struct cmark_pending_ref_user **pp = &s->pending_ref_users_head;
  while (*pp) {
    struct cmark_pending_ref_user *u = *pp;
    if (u->user_block == node) {
      *pp = u->next;
      cmark_chunk_free(parser->mem, &u->label);
      if (u->normalized_label)
        parser->mem->free(u->normalized_label);
      parser->mem->free(u);
    } else {
      pp = &u->next;
    }
  }
}

void cmark_parser_feed_add_pending_ref(cmark_parser *parser,
                                     cmark_chunk label,
                                     cmark_node *block) {
  if (!(parser->options & CMARK_OPT_FEED_AST))
    return;
  unsigned char *norm = normalize_map_label(parser->mem, &label);
  if (!norm)
    return;
  // Dedup: if (block, normalized_label) is already in the list, no-op. This
  // matters for repeated snapshots of an open block before its [foo]
  // definition arrives — without dedup, every reparse adds another entry.
  for (struct cmark_pending_ref_user *u = parser->feed.pending_ref_users_head;
       u; u = u->next) {
    if (u->user_block == block && u->normalized_label &&
        strcmp((const char *)u->normalized_label,
               (const char *)norm) == 0) {
      parser->mem->free(norm);
      return;
    }
  }
  struct cmark_pending_ref_user *u =
      (struct cmark_pending_ref_user *)parser->mem->calloc(
          1, sizeof(struct cmark_pending_ref_user));
  // Take ownership of a copy of label so the original storage may be freed.
  u->label.alloc = 1;
  u->label.len = label.len;
  u->label.data = (unsigned char *)parser->mem->calloc(1, (size_t)label.len + 1);
  if (label.len > 0)
    memcpy(u->label.data, label.data, label.len);
  u->normalized_label = norm;
  u->user_block = block;
  u->next = parser->feed.pending_ref_users_head;
  parser->feed.pending_ref_users_head = u;
}

void cmark_parser_feed_resolve_pending_refs(cmark_parser *parser,
                                          cmark_chunk label) {
  unsigned char *norm_resolved = normalize_map_label(parser->mem, &label);
  if (norm_resolved == NULL)
    return;

  // In tentative mode we mark matching user blocks dirty but leave the
  // pending entries in place. The pending list is not part of the txn's
  // undo log, so dropping entries here would leak them across the upcoming
  // revert: the def's REFMAP_ADD gets reversed, leaving the blocks once
  // again unresolved with no entry to re-trigger them when the canonical
  // def eventually arrives.
  bool consume = !cmark_parser_feed_partial_txn_active(parser);

  struct cmark_pending_ref_user **pp =
      &parser->feed.pending_ref_users_head;
  while (*pp) {
    struct cmark_pending_ref_user *u = *pp;
    bool match = u->normalized_label &&
                 strcmp((const char *)u->normalized_label,
                        (const char *)norm_resolved) == 0;
    if (match) {
      cmark_parser_feed_mark_inline_dirty(parser, u->user_block);
      if (consume) {
        *pp = u->next;
        cmark_chunk_free(parser->mem, &u->label);
        if (u->normalized_label)
          parser->mem->free(u->normalized_label);
        parser->mem->free(u);
        continue;
      }
    }
    pp = &u->next;
  }
  parser->mem->free(norm_resolved);
}

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

cmark_node *cmark_parser_snapshot(cmark_parser *parser) {
  // Strict precondition: snapshot only operates on parsers that opted into
  // feed mode at creation via CMARK_OPT_FEED_AST. Without that flag the
  // dirty-blocks list, partial-line txn, and pending-ref index are all
  // never populated — feeding bytes through this parser leaves no
  // bookkeeping for snapshot to consume, so any "snapshot" we returned
  // would be a tree with no inline children for committed paragraphs and
  // no tentative pass for the partial line. Returning NULL surfaces the
  // misuse instead of silently returning a wrong tree.
  if (!(parser->options & CMARK_OPT_FEED_AST))
    return NULL;

  // Idempotency: if the prior snapshot's tentative tree is still valid
  // (no feed has invalidated it), the tree has not changed, so just return
  // the same root. This skips the O(tree-size) flag-snapshot capture +
  // revert that would otherwise rebuild an identical tentative.
  if (parser->feed.tentative_clean &&
      cmark_parser_feed_partial_txn_active(parser)) {
    return parser->root;
  }

  cmark_parser *prev = cmark_parser_feed_active_parser();
  cmark_parser_feed_set_active_parser(parser);

  // Roll back any txn left open by a prior snapshot so we start from
  // canonical state.
  if (cmark_parser_feed_partial_txn_active(parser))
    cmark_parser_feed_partial_txn_revert(parser);

  // Phase 1: drain canonical dirty inlines (committed blocks marked dirty
  // by feed). These are persistent — not part of the tentative txn.
  cmark_parser_feed_run_pending_inlines(parser);

  // Phase 2: tentatively reach EOF-equivalent state. A single partial-line
  // txn covers (a) processing in-flight linebuf bytes through S_process_line
  // and (b) running per-block finalize on every open block. Without the
  // finalize step the snapshot tree diverges from cmark_parse_document(prefix):
  // ref-only paragraphs stay as plain text, fenced code blocks lack their
  // info string, lists never resolve tight/loose, etc. The txn is left
  // open; the next feed/finish/snapshot reverts it.
  cmark_parser_feed_partial_txn_begin(parser);
  cmark_parser_feed_process_partial_line(parser);
  cmark_parser_feed_tentative_finalize(parser);
  // Re-drain: finalize may have mutated paragraph content (ref-def
  // extraction) and marked blocks dirty for re-parse against the
  // truncated buffer.
  cmark_parser_feed_run_pending_inlines(parser);

  // Run extension postprocess hooks so the snapshot tree matches what
  // cmark_parser_finish would produce. This materializes things the
  // inline parser doesn't emit — e.g. GFM autolink turns "a@b.com" text
  // runs into <link> nodes, GFM strikethrough finalizes its delimiter
  // pairs. Without this, snapshot output diverges from the one-shot
  // oracle for any callers that attached extensions.
  //
  // Postprocess mutations are NOT logged in the partial-line txn — the
  // hooks generally call cmark_node_unlink / insert_after / free without
  // going through txn-tracked primitives, so per-mutation undo would
  // require a parallel journal. We rely instead on postprocess being
  // idempotent on its output and on the dirty-drain pipeline detaching+
  // reparsing any block whose canonical content actually changed before
  // the next snapshot — INLINE_PARSE op then frees the postprocess
  // output as part of the block's prior children chain.
  cmark_parser_extension_postprocess(parser);

  // Tentative tree now reflects the parser state that produced it; further
  // back-to-back snapshot calls will short-circuit until something
  // (feed / feed_reentrant / finish) calls revert and clears the flag.
  parser->feed.tentative_clean = true;

  cmark_parser_feed_set_active_parser(prev);
  return parser->root;
}

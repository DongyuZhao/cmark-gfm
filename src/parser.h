#ifndef CMARK_PARSER_H
#define CMARK_PARSER_H

#include <stdio.h>
#include "references.h"
#include "map.h"
#include "node.h"
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINK_LABEL_LENGTH 1000

/**
 * Per-parser feed state. Aggregated here (not allocated separately) to
 * keep cache locality with the rest of the parser. All fields are zero-init
 * compatible — a parser that never calls snapshot() pays only the storage
 * cost.
 */
struct cmark_parser_feed_state {
  /**
   * Head of the intrusive dirty-block list (linked via cmark_node.dirty_next).
   * Blocks are appended when their content changes and drained on snapshot.
   */
  struct cmark_node *dirty_blocks_head;
  /**
   * Reverse index: link reference labels seen as `[ref]`-style uses whose
   * definitions are not yet in refmap. Linked list of {label_chunk,
   * user_block} pairs. Drained when a definition arrives, marking the user
   * block inline-dirty for re-parse.
   */
  struct cmark_pending_ref_user *pending_ref_users_head;
  /**
   * Pending partial-line transaction. cmark_parser_snapshot runs
   * S_process_line on (linebuf + '\n') so the partial line surfaces in the
   * returned tree; the next call to feed / finish / snapshot reverts the
   * mutations recorded here. NULL when no tentative processing is in flight.
   */
  struct cmark_partial_line_txn *partial_txn;
  /**
   * Idempotency cache: true iff the current partial_txn reflects parser
   * state that has not changed since the last cmark_parser_snapshot call.
   * Set at the end of snapshot, cleared whenever revert tears down the txn
   * (which happens at the start of feed / feed_reentrant / finish, and at
   * the start of snapshot before re-running). When set on snapshot entry,
   * the call short-circuits and returns parser->root immediately —
   * avoiding the O(tree) flag-snapshot capture+restore that otherwise
   * dominates back-to-back snapshots on a large tree.
   */
  bool tentative_clean;
};

/**
 * Tentative-mutation op kinds. Each op type's payload is captured in
 * cmark_partial_line_txn_record; revert applies the inverse in reverse
 * chronological order.
 */
enum cmark_partial_line_txn_op {
  /**
   * A new node was attached as a child of an existing node during tentative
   * processing. Revert: cmark_node_free the new node.
   */
  CMARK_TXN_OP_ADD_CHILD,
  /**
   * A node's content buffer was extended via add_line during tentative.
   * Revert: truncate content back to prior size, clear inline state, mark
   * dirty.
   */
  CMARK_TXN_OP_ADD_LINE,
  /**
   * A node was rewritten in place (cmark_node_set_type) during tentative.
   * Revert: restore prior type and `as` payload; re-link any incompatible
   * children that morph detached.
   */
  CMARK_TXN_OP_MORPH,
  /**
   * A block was finalized (CMARK_NODE__OPEN cleared, end_line/end_column set,
   * type-specific finalize side effects applied) during tentative. Revert:
   * restore OPEN flag and prior end_line/end_column. The type-specific
   * finalize side effects (e.g. CODE_BLOCK detaching content into
   * as.code.literal, LIST setting tight) are captured by separate ops or by
   * MORPH if applicable.
   */
  CMARK_TXN_OP_CLOSE_BLOCK,
  /**
   * A reference definition was added to refmap during tentative (typically
   * via try_eager_ref_extract from add_line, or
   * resolve_reference_link_definitions inside finalize on the partial line).
   * Revert: unlink and free the cmark_map_entry; invalidate sorted cache.
   */
  CMARK_TXN_OP_REFMAP_ADD,
  /**
   * A node was unlinked from its parent during tentative — typically a
   * paragraph that turned out to be only ref-defs and was removed in
   * finalize. We never actually free the node during tentative (revert would
   * see dangling pointers); we only unlink and stash. Revert: re-link into
   * the recorded position; commit (= end of session) frees the stash.
   */
  CMARK_TXN_OP_DETACH_NODE,
  /**
   * A block's inline children were freed and rebuilt by an inline-parse run
   * during tentative. Revert: free the new children, restore prior
   * inline_parsed_len, mark dirty so the next parse pass rebuilds.
   */
  CMARK_TXN_OP_INLINE_PARSE,
  /**
   * A block's content buffer was modified by something other than add_line
   * (e.g. resolve_reference_link_definitions trimming consumed bytes,
   * CODE_BLOCK finalize detaching content into as.code.literal). Revert:
   * restore the saved content bytes.
   */
  CMARK_TXN_OP_CONTENT_REWRITE,
  /**
   * As-bytes-only snapshot, no type change, no children involvement. Used
   * by finalize to capture as-payload before type-specific finalize logic
   * mutates it. Revert: feed_free_as on the current as, then memcpy
   * prior_as_bytes back.
   */
  CMARK_TXN_OP_SAVE_AS,
};

struct cmark_partial_line_txn_record {
  enum cmark_partial_line_txn_op op;
  struct cmark_node *node;
  /* For ADD_LINE: prior content size. */
  bufsize_t prior_content_size;
  bufsize_t prior_inline_parsed_len;
  /* For MORPH: prior type and a copy of the prior `as` union plus any
   * detached children. The detached chain is kept alive (not freed) so
   * revert can re-link them; commit frees them via the txn destructor. */
  uint16_t prior_type;
  /* MORPH `as` payload bytes — copied by memcpy. The new value is in node.
   * Free responsibility: the union may carry chunks/strings that need
   * cmark_chunk_free etc.; we manage them via free_node_as_payload helpers
   * in feed.c. */
  unsigned char prior_as_bytes[64];
  bool prior_as_present;
  struct cmark_node *detached_children_first;
  struct cmark_node *detached_children_last;
  /* For CLOSE_BLOCK: prior open flag state and end position. */
  cmark_node_internal_flags prior_flags;
  int prior_end_line;
  int prior_end_column;
  /* For REFMAP_ADD: the entry that was inserted (head of map->refs after
   * insert). */
  struct cmark_map_entry *added_entry;
  /* For DETACH_NODE: where the node was attached so revert can re-link.
   * The node itself is held in `node`; its sibling pointers were already
   * updated by the unlink (S_node_unlink). We save reattachment anchors. */
  struct cmark_node *detach_parent;
  struct cmark_node *detach_prev;
  struct cmark_node *detach_next;
  /* For INLINE_PARSE: the prior inline-children chain (detached, alive). */
  struct cmark_node *prior_inline_first;
  struct cmark_node *prior_inline_last;
  /* For CONTENT_REWRITE: a saved snapshot of the prior content bytes. */
  unsigned char *prior_content_bytes;
  bufsize_t prior_content_bytes_len;
};

/**
 * Per-node side-channel snapshot taken at txn_begin. Captures every node
 * in the tree at that moment; on revert we restore the snapshot to undo
 * any mutation an extension makes outside the txn-tracked code paths
 * (memoization flags like CMARK_NODE__TABLE_VISITED, extension pointer
 * assignments via cmark_node_set_syntax_extension, etc.).
 */
struct cmark_node_flag_snapshot {
  struct cmark_node *node;
  cmark_node_internal_flags flags;
  cmark_syntax_extension *extension;
};

struct cmark_partial_line_txn {
  bool unrevertable;  // an unhandled mutation occurred; snapshot must skip
                      // partial-line tentative results.
  // Saved parser scalar state at txn begin:
  struct cmark_node *prior_current;
  int prior_line_number;
  bufsize_t prior_last_line_length;
  bool prior_blank;
  bool prior_partially_consumed_tab;
  bufsize_t prior_offset;
  bufsize_t prior_column;
  bufsize_t prior_first_nonspace;
  bufsize_t prior_first_nonspace_column;
  bufsize_t prior_thematic_break_kill_pos;
  int prior_indent;
  size_t prior_total_size;
  bool prior_last_buffer_ended_with_cr;
  // Mutation log (chronological).
  struct cmark_partial_line_txn_record *records;
  size_t records_len;
  size_t records_cap;
  // Pre-tentative flags snapshot: every node alive at txn_begin. On revert
  // we restore each node's flags so extension memoization (e.g.
  // CMARK_NODE__TABLE_VISITED) doesn't leak into the canonical pass.
  // Nodes added during tentative are tracked separately by ADD_CHILD and
  // are freed on revert, so they don't appear here.
  struct cmark_node_flag_snapshot *flag_snapshot;
  size_t flag_snapshot_len;
  size_t flag_snapshot_cap;
};

struct cmark_pending_ref_user {
  cmark_chunk label;
  unsigned char *normalized_label; // for dedup; owned, freed with the entry.
  struct cmark_node *user_block;
  struct cmark_pending_ref_user *next;
};

/**
 * Returns true if `node` is the kind of block that has inline children
 * (e.g. paragraph, heading, or any extension block whose
 * contains_inlines_func returns true). Defined in blocks.c; declared here
 * so feed.c can share the predicate without duplicating it.
 */
bool cmark_block_contains_inlines(struct cmark_node *node);

/**
 * Run every attached syntax extension's postprocess hook against the
 * current parser->root, just like cmark_parser_finish does at the end of
 * a one-shot parse. A hook may return a replacement root (wrapping the
 * document in a new node); the new pointer is captured back into
 * parser->root.
 *
 * Both finish and feed-mode snapshot call this so their output trees
 * agree on what extension-driven post-passes (e.g. GFM autolink turning
 * "a@b.com" into <link>, strikethrough finalizing delimiter pairs)
 * have produced. Postprocess mutations are NOT logged in the
 * partial-line txn; correctness in feed mode relies on hooks being
 * idempotent and on the INLINE_PARSE op freeing per-block postprocess
 * output when a block is later reparsed.
 */
void cmark_parser_extension_postprocess(struct cmark_parser *parser);

struct cmark_parser {
  struct cmark_mem *mem;
  /* A hashtable of urls in the current document for cross-references */
  struct cmark_map *refmap;
  /* The root node of the parser, always a CMARK_NODE_DOCUMENT */
  struct cmark_node *root;
  /* The last open block after a line is fully processed */
  struct cmark_node *current;
  /* See the documentation for cmark_parser_get_line_number() in cmark.h */
  int line_number;
  /* See the documentation for cmark_parser_get_offset() in cmark.h */
  bufsize_t offset;
  /* See the documentation for cmark_parser_get_column() in cmark.h */
  bufsize_t column;
  /* See the documentation for cmark_parser_get_first_nonspace() in cmark.h */
  bufsize_t first_nonspace;
  /* See the documentation for cmark_parser_get_first_nonspace_column() in cmark.h */
  bufsize_t first_nonspace_column;
  bufsize_t thematic_break_kill_pos;
  /* See the documentation for cmark_parser_get_indent() in cmark.h */
  int indent;
  /* See the documentation for cmark_parser_is_blank() in cmark.h */
  bool blank;
  /* See the documentation for cmark_parser_has_partially_consumed_tab() in cmark.h */
  bool partially_consumed_tab;
  /* Contains the currently processed line */
  cmark_strbuf curline;
  /* See the documentation for cmark_parser_get_last_line_length() in cmark.h */
  bufsize_t last_line_length;
  /* FIXME: not sure about the difference with curline */
  cmark_strbuf linebuf;
  /* Options set by the user, see the Options section in cmark.h */
  int options;
  bool last_buffer_ended_with_cr;
  size_t total_size;
  cmark_llist *syntax_extensions;
  cmark_llist *inline_syntax_extensions;
  cmark_ispunct_func backslash_ispunct;
  /* Feed-driven AST state. Always present; quiescent unless feed mode is
   * activated (CMARK_OPT_FEED_AST or first cmark_parser_snapshot call). */
  struct cmark_parser_feed_state feed;
};

#ifdef __cplusplus
}
#endif

#endif

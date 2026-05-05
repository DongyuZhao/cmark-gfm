#ifndef CMARK_FEED_H
#define CMARK_FEED_H

#include "cmark-gfm.h"
#include "node.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize/free per-parser feed state. Called by cmark_parser_reset
 * and cmark_parser_dispose.
 */
void cmark_parser_feed_state_init(struct cmark_parser_feed_state *s);
void cmark_parser_feed_state_free(cmark_mem *mem,
                                struct cmark_parser_feed_state *s);

/**
 * Set/get the parser that is currently consuming input on this thread. Used
 * so that mutation primitives like cmark_node_set_type and
 * cmark_reference_create can record txn entries and trigger pending-ref
 * resolution without taking the parser as a parameter (which would change
 * public ABIs).
 *
 * Implemented as a thread-local: each thread may drive its own parser.
 */
void cmark_parser_feed_set_active_parser(cmark_parser *p);
cmark_parser *cmark_parser_feed_active_parser(void);

/**
 * Drain the dirty-block list, running incremental inline parsing on each
 * block whose contains_inlines() returns true. Free existing inline
 * children, call cmark_parse_inlines, update inline_parsed_len, clear
 * INLINE_DIRTY. Called by cmark_parser_snapshot and (via
 * process_inlines() in blocks.c) at finalize time so an already-parsed
 * subtree is not re-parsed.
 */
void cmark_parser_feed_run_pending_inlines(cmark_parser *parser);

/**
 * Mark a block's inline content as dirty, queuing it for re-parse on the
 * next snapshot. Idempotent. No-op when feed bookkeeping is inactive
 * (cmark_parse_document path) — the finalize-time fall-back compares
 * inline_parsed_len < content.size, which is sufficient on its own.
 */
void cmark_parser_feed_mark_inline_dirty(cmark_parser *parser, cmark_node *block);

/**
 * Drain the dirty-block list. Returns the head; caller must walk via
 * dirty_next and reset each node's CMARK_NODE__INLINE_DIRTY flag and
 * dirty_next pointer.
 */
cmark_node *cmark_parser_feed_drain_dirty_blocks(cmark_parser *parser);

/**
 * Tentatively process the parser's in-flight `linebuf` as a complete line so
 * the snapshot tree reflects bytes fed since the last newline. Drives the
 * partial-line transaction: every mutation made by the underlying
 * S_process_line + inline-parse run is logged so the next call to feed,
 * finish, or snapshot can undo it. If a mutation occurs that we have no
 * undo for, the txn marks itself unrevertable; the caller (snapshot) then
 * reverts whatever it can and the resulting tree omits the partial line.
 * Implemented in blocks.c so it can reach S_process_line.
 *
 * Caller must own the partial-line txn lifecycle (begin before, revert/
 * leave-open after); this function only logs into whatever txn is active.
 */
void cmark_parser_feed_process_partial_line(cmark_parser *parser);

/**
 * Tentatively run EOF-style finalize on every open block: walks
 * parser->current up to root invoking the same per-block finalize the
 * batch path uses. Required for snapshot equivalence with
 * cmark_parse_document(prefix) — without it, paragraph-only ref defs
 * stay as plain text, fenced code blocks lack their info string, lists
 * never resolve tight/loose, etc. Mutations are logged into the active
 * partial-line txn for revert. Implemented in blocks.c so it can reach
 * the file-static finalize().
 */
void cmark_parser_feed_tentative_finalize(cmark_parser *parser);

/**
 * Partial-line transaction — begin/active/revert. cmark_parser_snapshot calls
 * begin, runs S_process_line on (linebuf + '\n'), then leaves the
 * transaction open. The next call to feed, finish, or snapshot calls revert
 * to undo the tentative tree mutations. Active() reports true between
 * begin() and revert(). Mark_unrevertable flags a mutation that has no
 * recorded undo; revert then restores what it can and bails on the rest.
 */
void cmark_parser_feed_partial_txn_begin(cmark_parser *parser);
bool cmark_parser_feed_partial_txn_active(cmark_parser *parser);
void cmark_parser_feed_partial_txn_mark_unrevertable(cmark_parser *parser);
void cmark_parser_feed_partial_txn_revert(cmark_parser *parser);

/**
 * Per-op record helpers. All are no-ops outside an active txn so the call
 * sites can invoke them unconditionally during parsing. Each op corresponds
 * to a cmark_partial_line_txn_op enum value; see parser.h for the revert
 * semantics.
 */
void cmark_parser_feed_partial_txn_record_add_child(cmark_parser *parser,
                                                  cmark_node *child);
void cmark_parser_feed_partial_txn_record_add_line(cmark_parser *parser,
                                                 cmark_node *node,
                                                 bufsize_t prior_size);
/**
 * Record an in-place type change. `prior_as_bytes` is a copy of the prior
 * `as` union bytes (sizeof(node->as)). `detached_first/last` is the chain
 * of children that the morph detached because the new type cannot contain
 * them — they are kept alive (not freed) so revert can re-link them.
 */
void cmark_parser_feed_partial_txn_record_morph(cmark_parser *parser,
                                              cmark_node *node,
                                              uint16_t prior_type,
                                              const void *prior_as_bytes,
                                              size_t prior_as_size,
                                              cmark_node *detached_first,
                                              cmark_node *detached_last);
/**
 * Record an as-bytes-only snapshot (no type change, no children touched).
 * Used by finalize when a block's as union is about to be populated by
 * type-specific finalize logic (CODE_BLOCK literal/info chunks, LIST
 * tight flag, etc.).
 */
void cmark_parser_feed_partial_txn_record_save_as(cmark_parser *parser,
                                                cmark_node *node,
                                                const void *prior_as_bytes,
                                                size_t prior_as_size);
/**
 * Record a block close (CMARK_NODE__OPEN flag clear, end_line/end_column
 * update). Caller passes the values that were in place before the close.
 */
void cmark_parser_feed_partial_txn_record_close_block(cmark_parser *parser,
                                                    cmark_node *node,
                                                    cmark_node_internal_flags prior_flags,
                                                    int prior_end_line,
                                                    int prior_end_column);
/**
 * Record a refmap insertion. `entry` is the cmark_map_entry that was added
 * (typically map->refs immediately after the insert).
 */
void cmark_parser_feed_partial_txn_record_refmap_add(cmark_parser *parser,
                                                   struct cmark_map_entry *entry);
/**
 * Record a node unlink (used during tentative finalize for ref-def-only
 * paragraph removal). The node is kept alive in a detached state; revert
 * reattaches at (parent, prev, next).
 */
void cmark_parser_feed_partial_txn_record_detach(cmark_parser *parser,
                                               cmark_node *node,
                                               cmark_node *parent,
                                               cmark_node *prev,
                                               cmark_node *next);
/**
 * Record an inline-parse run on a block. `prior_first/last` is the inline
 * children chain that existed before the parse (already detached by the
 * caller and kept alive). `prior_parsed_len` is the block's
 * inline_parsed_len before the run.
 */
void cmark_parser_feed_partial_txn_record_inline_parse(cmark_parser *parser,
                                                     cmark_node *block,
                                                     cmark_node *prior_first,
                                                     cmark_node *prior_last,
                                                     bufsize_t prior_parsed_len);
/**
 * Record an arbitrary content-buffer rewrite. `prior_bytes` of length
 * `prior_len` is duplicated into the txn record so the caller can mutate
 * node->content immediately after.
 */
void cmark_parser_feed_partial_txn_record_content_rewrite(cmark_parser *parser,
                                                        cmark_node *node,
                                                        const unsigned char *prior_bytes,
                                                        bufsize_t prior_len);

/**
 * Drop any references the feed state holds to `node` before it is
 * freed. Cleans up the dirty-blocks list, pending-ref users registered
 * against the node, and detached / morph-stashed entries in any active
 * partial-line txn.
 */
void cmark_parser_feed_forget_node(cmark_parser *parser, cmark_node *node);

/**
 * Add a pending [ref] user — when `block` contains an unresolved reference
 * of `label`, the block must be re-inline-parsed once the definition
 * arrives. Idempotent on (block, normalized_label) — repeated registration
 * across snapshots before the def arrives does not accumulate duplicates.
 */
void cmark_parser_feed_add_pending_ref(cmark_parser *parser,
                                     cmark_chunk label,
                                     cmark_node *block);
/**
 * Notify that a definition for `label` has arrived. All pending users are
 * removed from the index and queued for inline-reparse via
 * cmark_parser_feed_mark_inline_dirty.
 */
void cmark_parser_feed_resolve_pending_refs(cmark_parser *parser,
                                          cmark_chunk label);

#ifdef __cplusplus
}
#endif

#endif

#include "cmark-gfm.h"
#include "parser.h"
#include "references.h"
#include "inlines.h"
#include "chunk.h"
#include "feed.h"

static void reference_free(cmark_map *map, cmark_map_entry *_ref) {
  cmark_reference *ref = (cmark_reference *)_ref;
  cmark_mem *mem = map->mem;
  if (ref != NULL) {
    mem->free(ref->entry.label);
    cmark_chunk_free(mem, &ref->url);
    cmark_chunk_free(mem, &ref->title);
    mem->free(ref);
  }
}

void cmark_reference_create(cmark_map *map, cmark_chunk *label,
                            cmark_chunk *url, cmark_chunk *title) {
  cmark_reference *ref;
  unsigned char *reflabel = normalize_map_label(map->mem, label);

  /* empty reference name, or composed from only whitespace */
  if (reflabel == NULL)
    return;

  // Feed: refs may be added after a previous lookup has populated the
  // sorted-index cache (this never happens in batch mode, where defs are
  // fully discovered before any inline parsing). Invalidate so the next
  // lookup re-sorts and observes the new entry.
  if (map->sorted) {
    map->mem->free(map->sorted);
    map->sorted = NULL;
  }

  ref = (cmark_reference *)map->mem->calloc(1, sizeof(*ref));
  ref->entry.label = reflabel;
  ref->url = cmark_clean_url(map->mem, url);
  ref->title = cmark_clean_title(map->mem, title);
  ref->entry.age = map->size;
  ref->entry.next = map->refs;
  ref->entry.size = ref->url.len + ref->title.len;

  map->refs = (cmark_map_entry *)ref;
  map->size++;

  // Feed hooks. Cross-check that the active parser owns this map —
  // public callers may operate on a standalone map outside any parser
  // context.
  {
    cmark_parser *active = cmark_parser_feed_active_parser();
    if (active && active->refmap == map) {
      // Tentative: log the insertion so revert can remove it. This must
      // come BEFORE resolve_pending_refs, since that function would
      // re-mark blocks dirty — and reverting REFMAP_ADD without unwinding
      // the dirty-mark would still be safe (re-parsing without the def
      // produces the same result), but we keep the order to match the
      // chronological undo invariant.
      if (cmark_parser_feed_partial_txn_active(active)) {
        cmark_parser_feed_partial_txn_record_refmap_add(active,
                                                      &ref->entry);
      }
      // Notify any blocks that previously failed to resolve this label so
      // they get re-parsed on the next snapshot.
      cmark_parser_feed_resolve_pending_refs(active, *label);
    }
  }
}

cmark_map *cmark_reference_map_new(cmark_mem *mem) {
  return cmark_map_new(mem, reference_free);
}

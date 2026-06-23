#ifndef CMARK_GFM_CORE_EXTENSIONS_H
#define CMARK_GFM_CORE_EXTENSIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cmark-gfm-extension_api.h"
#include "cmark-gfm_export.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  CMARK_FORMULA_MODE_NONE = 0,
  CMARK_FORMULA_MODE_EMBEDDED,
  CMARK_FORMULA_MODE_STANDALONE
} cmark_formula_mode;

CMARK_GFM_EXPORT
void cmark_gfm_core_extensions_ensure_registered(void);

CMARK_GFM_EXPORT
uint16_t cmark_gfm_extensions_get_table_columns(cmark_node *node);

/** Sets the number of columns for the table, returning 1 on success and 0 on error.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_table_columns(cmark_node *node, uint16_t n_columns);

CMARK_GFM_EXPORT
uint8_t *cmark_gfm_extensions_get_table_alignments(cmark_node *node);

/** Sets the alignments for the table, returning 1 on success and 0 on error.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_table_alignments(cmark_node *node, uint16_t ncols, uint8_t *alignments);

CMARK_GFM_EXPORT
int cmark_gfm_extensions_get_table_row_is_header(cmark_node *node);

/** Sets whether the node is a table header row, returning 1 on success and 0 on error.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_table_row_is_header(cmark_node *node, int is_header);

CMARK_GFM_EXPORT
bool cmark_gfm_extensions_get_tasklist_item_checked(cmark_node *node);
/* For backwards compatibility */
#define cmark_gfm_extensions_tasklist_is_checked cmark_gfm_extensions_get_tasklist_item_checked

/** Sets whether a tasklist item is "checked" (completed), returning 1 on success and 0 on error.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_tasklist_item_checked(cmark_node *node, bool is_checked);

/** Returns the literal formula payload for formula extension nodes, or NULL on error.
 */
CMARK_GFM_EXPORT
const char *cmark_gfm_extensions_get_formula_literal(cmark_node *node);

/** Sets the literal formula payload for formula extension nodes, returning 1 on success and 0 on error.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_formula_literal(cmark_node *node, const char *literal);

/** Returns the paragraph-internal layout mode for formula extension nodes.
 */
CMARK_GFM_EXPORT
cmark_formula_mode cmark_gfm_extensions_get_formula_mode(cmark_node *node);

/** Sets the paragraph-internal layout mode for formula extension nodes.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_formula_mode(cmark_node *node,
                                       cmark_formula_mode mode);

/** Returns the directive name for directive extension nodes, or NULL on
 * error.
 */
CMARK_GFM_EXPORT
const char *cmark_gfm_extensions_get_directive_name(cmark_node *node);

/** Sets the directive name for directive extension nodes, returning 1
 * on success and 0 on error.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_directive_name(cmark_node *node,
                                            const char *name);

/** Returns normalized HTML attributes for directive extension nodes, or
 * NULL on error.
 */
CMARK_GFM_EXPORT
const char *
cmark_gfm_extensions_get_directive_attributes(cmark_node *node);

/** Sets normalized HTML attributes for directive extension nodes,
 * returning 1 on success and 0 on error.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_directive_attributes(cmark_node *node,
                                                  const char *attributes);

/** Returns the citation ref payload for MS Copilot citation extension nodes, or NULL on error.
 */
CMARK_GFM_EXPORT
const char *cmark_gfm_extensions_get_ms_copilot_citation_ref(cmark_node *node);

/** Sets the citation ref payload for MS Copilot citation extension nodes, returning 1 on success and 0 on error.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_ms_copilot_citation_ref(cmark_node *node, const char *ref);

/** Returns the scenario payload for MS Copilot annotation extension nodes, or NULL on error.
 */
CMARK_GFM_EXPORT
const char *cmark_gfm_extensions_get_ms_copilot_annotation_scenario(cmark_node *node);

/** Sets the scenario payload for MS Copilot annotation extension nodes, returning 1 on success and 0 on error.
 */
CMARK_GFM_EXPORT
int cmark_gfm_extensions_set_ms_copilot_annotation_scenario(cmark_node *node,
                                                            const char *scenario);

#ifdef __cplusplus
}
#endif

#endif

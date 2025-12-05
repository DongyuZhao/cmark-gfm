#ifndef CMARK_GFM_FORMULA_H
#define CMARK_GFM_FORMULA_H

#include "cmark-gfm-core-extensions.h"

extern cmark_node_type CMARK_NODE_INLINE_FORMULA;
extern cmark_node_type CMARK_NODE_FORMULA_BLOCK;

cmark_syntax_extension *create_formula_extension(void);

CMARK_GFM_EXPORT
const char *cmark_gfm_extensions_get_formula_literal(cmark_node *node);

#endif

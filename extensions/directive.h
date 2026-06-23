#ifndef CMARK_GFM_DIRECTIVE_H
#define CMARK_GFM_DIRECTIVE_H

#include "cmark-gfm-core-extensions.h"

extern cmark_node_type CMARK_NODE_DIRECTIVE;
extern cmark_node_type CMARK_NODE_DIRECTIVE_LEAF;
extern cmark_node_type CMARK_NODE_DIRECTIVE_CONTAINER;
extern cmark_node_type CMARK_NODE_DIRECTIVE_LABEL;

cmark_syntax_extension *create_directive_extension(void);

#endif

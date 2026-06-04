#ifndef CMARK_GFM_MS_COPILOT_ACCORDION_H
#define CMARK_GFM_MS_COPILOT_ACCORDION_H

#include "cmark-gfm-core-extensions.h"

extern cmark_node_type CMARK_NODE_MS_COPILOT_ACCORDION;
extern cmark_node_type CMARK_NODE_MS_COPILOT_ACCORDION_HEADER;
extern cmark_node_type CMARK_NODE_MS_COPILOT_ACCORDION_CONTENT;

cmark_syntax_extension *create_ms_copilot_accordion_extension(void);

#endif

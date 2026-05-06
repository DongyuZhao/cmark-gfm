#ifndef CMARK_INLINES_H
#define CMARK_INLINES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "references.h"

cmark_chunk cmark_clean_url(cmark_mem *mem, cmark_chunk *url);
cmark_chunk cmark_clean_title(cmark_mem *mem, cmark_chunk *title);

CMARK_GFM_EXPORT
void cmark_parse_inlines(cmark_parser *parser,
                         cmark_node *parent,
                         cmark_map *refmap,
                         int options);

// Feed: resume-aware inline parse. start_offset > 0 indicates the
// existing children of `parent` are stable up to that byte offset of
// `parent->content`, and the parser should append children for
// content[start_offset..] only. Caller must ensure
// CMARK_NODE__INLINE_CLEAN_END was set by the prior parse before invoking
// with a non-zero offset; otherwise the resume is unsafe (a delimiter
// left dangling in the prefix could be closed by the new content).
// start_offset == 0 is equivalent to cmark_parse_inlines (full reparse).
void cmark_parse_inlines_resume(cmark_parser *parser,
                                cmark_node *parent,
                                cmark_map *refmap,
                                int options,
                                bufsize_t start_offset);

bufsize_t cmark_parse_reference_inline(cmark_mem *mem, cmark_chunk *input,
                                       cmark_map *refmap);

void cmark_inlines_add_special_character(unsigned char c, bool emphasis);
void cmark_inlines_remove_special_character(unsigned char c, bool emphasis);

#ifdef __cplusplus
}
#endif

#endif

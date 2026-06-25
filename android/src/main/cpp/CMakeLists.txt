cmake_minimum_required(VERSION 3.22.1)
project(cmark_gfm_android LANGUAGES C)

set(CMARK_GFM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." CACHE PATH "Path to the cmark-gfm repository root")
get_filename_component(CMARK_GFM_ROOT "${CMARK_GFM_ROOT}" ABSOLUTE)
set(CMARK_GFM_PUBLIC_INCLUDE_DIR "${CMARK_GFM_ROOT}/android/src/main/prefab/cmark_gfm/include")

add_library(cmark_gfm SHARED
  "${CMARK_GFM_ROOT}/src/cmark.c"
  "${CMARK_GFM_ROOT}/src/node.c"
  "${CMARK_GFM_ROOT}/src/iterator.c"
  "${CMARK_GFM_ROOT}/src/blocks.c"
  "${CMARK_GFM_ROOT}/src/inlines.c"
  "${CMARK_GFM_ROOT}/src/scanners.c"
  "${CMARK_GFM_ROOT}/src/utf8.c"
  "${CMARK_GFM_ROOT}/src/buffer.c"
  "${CMARK_GFM_ROOT}/src/references.c"
  "${CMARK_GFM_ROOT}/src/footnotes.c"
  "${CMARK_GFM_ROOT}/src/map.c"
  "${CMARK_GFM_ROOT}/src/render.c"
  "${CMARK_GFM_ROOT}/src/man.c"
  "${CMARK_GFM_ROOT}/src/xml.c"
  "${CMARK_GFM_ROOT}/src/html.c"
  "${CMARK_GFM_ROOT}/src/commonmark.c"
  "${CMARK_GFM_ROOT}/src/plaintext.c"
  "${CMARK_GFM_ROOT}/src/latex.c"
  "${CMARK_GFM_ROOT}/src/houdini_href_e.c"
  "${CMARK_GFM_ROOT}/src/houdini_html_e.c"
  "${CMARK_GFM_ROOT}/src/houdini_html_u.c"
  "${CMARK_GFM_ROOT}/src/cmark_ctype.c"
  "${CMARK_GFM_ROOT}/src/arena.c"
  "${CMARK_GFM_ROOT}/src/linked_list.c"
  "${CMARK_GFM_ROOT}/src/syntax_extension.c"
  "${CMARK_GFM_ROOT}/src/registry.c"
  "${CMARK_GFM_ROOT}/src/plugin.c"
  "${CMARK_GFM_ROOT}/extensions/core-extensions.c"
  "${CMARK_GFM_ROOT}/extensions/table.c"
  "${CMARK_GFM_ROOT}/extensions/ms_copilot_accordion.c"
  "${CMARK_GFM_ROOT}/extensions/ms_copilot_annotation.c"
  "${CMARK_GFM_ROOT}/extensions/ms_copilot_citation.c"
  "${CMARK_GFM_ROOT}/extensions/strikethrough.c"
  "${CMARK_GFM_ROOT}/extensions/autolink.c"
  "${CMARK_GFM_ROOT}/extensions/tagfilter.c"
  "${CMARK_GFM_ROOT}/extensions/formula.c"
  "${CMARK_GFM_ROOT}/extensions/directive.c"
  "${CMARK_GFM_ROOT}/extensions/ext_scanners.c"
  "${CMARK_GFM_ROOT}/extensions/tasklist.c"
)

target_include_directories(cmark_gfm
  PUBLIC
    "${CMARK_GFM_PUBLIC_INCLUDE_DIR}"
  PRIVATE
    "${CMARK_GFM_ROOT}/src"
    "${CMARK_GFM_ROOT}/extensions"
    "${CMARK_GFM_ROOT}/spm/include"
)

target_compile_definitions(cmark_gfm
  PRIVATE
    CMARK_GFM_EXPORTS
)

target_compile_options(cmark_gfm
  PRIVATE
    $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:GNU>>:-Wall>
    $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:GNU>>:-Wextra>
    $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:GNU>>:-Wno-unused-parameter>
    $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:GNU>>:-pedantic>
)

set_target_properties(cmark_gfm PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME "cmark_gfm"
)

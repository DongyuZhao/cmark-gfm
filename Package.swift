// swift-tools-version: 5.7
import PackageDescription

let package = Package(
    name: "cmark-gfm",
    products: [
        .library(name: "cmark-gfm", targets: ["cmark_gfm"]),
    ],
    targets: [
        .target(
            name: "cmark_gfm",
            path: ".",
            sources: [
                "src/cmark.c",
                "src/node.c",
                "src/iterator.c",
                "src/blocks.c",
                "src/inlines.c",
                "src/scanners.c",
                "src/utf8.c",
                "src/buffer.c",
                "src/references.c",
                "src/footnotes.c",
                "src/map.c",
                "src/render.c",
                "src/man.c",
                "src/xml.c",
                "src/html.c",
                "src/commonmark.c",
                "src/plaintext.c",
                "src/latex.c",
                "src/houdini_href_e.c",
                "src/houdini_html_e.c",
                "src/houdini_html_u.c",
                "src/cmark_ctype.c",
                "src/arena.c",
                "src/linked_list.c",
                "src/syntax_extension.c",
                "src/registry.c",
                "src/plugin.c",
                "extensions/core-extensions.c",
                "extensions/table.c",
                "extensions/ms_copilot_accordion.c",
                "extensions/ms_copilot_annotation.c",
                "extensions/ms_copilot_citation.c",
                "extensions/strikethrough.c",
                "extensions/autolink.c",
                "extensions/tagfilter.c",
                "extensions/formula.c",
                "extensions/directive.c",
                "extensions/ext_scanners.c",
                "extensions/tasklist.c",
            ],
            publicHeadersPath: "spm/include",
            cSettings: [
                .headerSearchPath("src"),
                .headerSearchPath("spm/include"),
                .define("CMARK_GFM_STATIC_DEFINE"),
                .define("CMARK_GFM_EXTENSIONS_STATIC_DEFINE"),
            ]
        ),
    ],
    cLanguageStandard: .c99
)

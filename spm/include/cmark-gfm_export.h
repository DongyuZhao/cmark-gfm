#ifndef CMARK_GFM_EXPORT_H
#define CMARK_GFM_EXPORT_H

#ifdef CMARK_GFM_STATIC_DEFINE
#define CMARK_GFM_EXPORT
#define CMARK_GFM_NO_EXPORT
#else
#ifndef CMARK_GFM_EXPORT
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(libcmark_gfm_EXPORTS)
#define CMARK_GFM_EXPORT __declspec(dllexport)
#else
#define CMARK_GFM_EXPORT __declspec(dllimport)
#endif
#else
#define CMARK_GFM_EXPORT __attribute__((visibility("default")))
#endif
#endif

#ifndef CMARK_GFM_NO_EXPORT
#define CMARK_GFM_NO_EXPORT __attribute__((visibility("hidden")))
#endif
#endif

#ifndef CMARK_GFM_DEPRECATED
#define CMARK_GFM_DEPRECATED __attribute__((__deprecated__))
#endif

#ifndef CMARK_GFM_DEPRECATED_EXPORT
#define CMARK_GFM_DEPRECATED_EXPORT CMARK_GFM_EXPORT CMARK_GFM_DEPRECATED
#endif

#ifndef CMARK_GFM_DEPRECATED_NO_EXPORT
#define CMARK_GFM_DEPRECATED_NO_EXPORT CMARK_GFM_NO_EXPORT CMARK_GFM_DEPRECATED
#endif

#endif

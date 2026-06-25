#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAILED=0

note_failure() {
  echo "android-prefab-check: $1" >&2
  FAILED=1
}

require_file() {
  if [ ! -f "$ROOT_DIR/$1" ]; then
    note_failure "missing $1"
  fi
}

require_text() {
  if [ ! -f "$ROOT_DIR/$1" ]; then
    return
  fi
  if ! grep -Fq "$2" "$ROOT_DIR/$1"; then
    note_failure "$1 does not contain: $2"
  fi
}

require_same_file() {
  if [ ! -f "$ROOT_DIR/$1" ] || [ ! -f "$ROOT_DIR/$2" ]; then
    return
  fi
  if ! cmp -s "$ROOT_DIR/$1" "$ROOT_DIR/$2"; then
    note_failure "$2 is not in sync with $1"
  fi
}

require_file "settings.gradle.kts"
require_file "build.gradle.kts"
require_file "android/build.gradle.kts"
require_file "android/src/main/AndroidManifest.xml"
require_file "android/src/main/cpp/CMakeLists.txt"

require_text "settings.gradle.kts" "include(\":android\")"
require_text "build.gradle.kts" "com.android.library"
require_text "android/build.gradle.kts" "prefabPublishing = true"
require_text "android/build.gradle.kts" "create(\"cmark_gfm\")"
require_text "android/build.gradle.kts" "headers = \"src/main/prefab/cmark_gfm/include\""
require_text "android/src/main/cpp/CMakeLists.txt" "add_library(cmark_gfm SHARED"

require_file "android/src/main/prefab/cmark_gfm/include/cmark-gfm.h"
require_file "android/src/main/prefab/cmark_gfm/include/cmark-gfm-extension_api.h"
require_file "android/src/main/prefab/cmark_gfm/include/cmark-gfm-core-extensions.h"
require_file "android/src/main/prefab/cmark_gfm/include/cmark-gfm_export.h"
require_file "android/src/main/prefab/cmark_gfm/include/cmark-gfm_version.h"
require_file "android/src/main/prefab/cmark_gfm/include/config.h"

require_same_file "src/cmark-gfm.h" "android/src/main/prefab/cmark_gfm/include/cmark-gfm.h"
require_same_file "src/cmark-gfm-extension_api.h" "android/src/main/prefab/cmark_gfm/include/cmark-gfm-extension_api.h"
require_same_file "extensions/cmark-gfm-core-extensions.h" "android/src/main/prefab/cmark_gfm/include/cmark-gfm-core-extensions.h"
require_same_file "spm/include/cmark-gfm_export.h" "android/src/main/prefab/cmark_gfm/include/cmark-gfm_export.h"
require_same_file "spm/include/cmark-gfm_version.h" "android/src/main/prefab/cmark_gfm/include/cmark-gfm_version.h"
require_same_file "spm/include/config.h" "android/src/main/prefab/cmark_gfm/include/config.h"

if [ "$FAILED" -ne 0 ]; then
  exit 1
fi

if command -v cmake >/dev/null 2>&1; then
  BUILD_DIR="${TMPDIR:-/tmp}/cmark-gfm-android-prefab-check"
  cmake -S "$ROOT_DIR/android/src/main/cpp" -B "$BUILD_DIR" -DCMARK_GFM_ROOT="$ROOT_DIR"
  cmake --build "$BUILD_DIR" --target cmark_gfm
fi

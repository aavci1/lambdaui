#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LAMBDA_INCLUDE_DIR="include"
LAMBDA_SOURCE_DIR="src"
LAMBDA_TESTS_DIR="tests"

failures=()

add_failure() {
  failures+=("$1")
}

require_dir() {
  local path="$1"
  if [[ ! -d "$path" ]]; then
    printf 'required scan directory does not exist: %s\n' "$path" >&2
    exit 1
  fi
}

require_dir "$LAMBDA_INCLUDE_DIR"
require_dir "$LAMBDA_SOURCE_DIR"
require_dir "$LAMBDA_TESTS_DIR"

is_allowed_header_only_class() {
  case "$1" in
    Animation|AnimationBase|AnimationClipState|Bindable|Clipboard|Computed|Effect|EnvironmentBinding|EnvironmentEntry|EnvironmentSlot|ForView|HookInteractionSignalScope|HookLayoutScope|Interaction|PreparedRenderOps|RasterizedLayerCache|Renderer|Scope|ScopedInteractionScopeKey|ScopedTimer|ScopedWindowCreationModalParent|ShowView|Signal|SmallFn|SmallVector|SwitchView)
      return 0
      ;;
  esac
  return 1
}

is_allowed_forward_without_definition() {
  case "$1" in
    GestureTracker|Interaction|PreparedRenderOps|Renderer|TextSystem)
      return 0
      ;;
  esac
  return 1
}

has_definition() {
  local type="$1"
  rg -U -q "(?s)(^|[[:space:]])(class|struct)[[:space:]]+([A-Za-z0-9_]+::)*${type}\\b[^;{}]*\\{" "$LAMBDA_INCLUDE_DIR" "$LAMBDA_SOURCE_DIR"
}

has_out_of_class_implementation() {
  local type="$1"
  rg -q "(^|[[:space:]~:])${type}::" "$LAMBDA_INCLUDE_DIR" "$LAMBDA_SOURCE_DIR"
}

for stale_path in \
  "$LAMBDA_INCLUDE_DIR/Lambda/UI/SceneBuilder.hpp" \
  "$LAMBDA_SOURCE_DIR/UI/SceneBuilder/MeasureLayoutCache.hpp" \
  "$LAMBDA_SOURCE_DIR/UI/SceneBuilder" \
  "$LAMBDA_TESTS_DIR/SceneBuilderLayoutTests.cpp" \
  "$LAMBDA_TESTS_DIR/SceneBuilderReuseTests.cpp" \
  "$LAMBDA_TESTS_DIR/SceneBuilderTestSupport.hpp" \
  "$LAMBDA_TESTS_DIR/SceneGeometryIndexTests.cpp" \
  "$LAMBDA_TESTS_DIR/SemanticThemeTests.cpp"; do
  if [[ -e "$stale_path" ]]; then
    add_failure "removed SceneBuilder artifact still exists: $stale_path"
  fi
done

if rg -n "MeasureLayoutCache|SceneBuilder|SceneBuilderTestSupport|lambda/include/Lambda/UI/SceneBuilder\\.hpp|lambda/src/UI/SceneBuilder" "$LAMBDA_INCLUDE_DIR" "$LAMBDA_SOURCE_DIR" "$LAMBDA_TESTS_DIR" CMakeLists.txt README.md >/tmp/lambda-stale-symbols.$$ 2>/dev/null; then
  while IFS= read -r line; do
    add_failure "stale SceneBuilder/MeasureLayoutCache reference: $line"
  done </tmp/lambda-stale-symbols.$$
fi
rm -f /tmp/lambda-stale-symbols.$$

while IFS= read -r source; do
  if ! rg -q --fixed-strings "$(basename "$source")" -g CMakeLists.txt .; then
    add_failure "implementation source is not listed in CMakeLists.txt: $source"
  fi
done < <(find "$LAMBDA_SOURCE_DIR" -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.mm' \) | sort)

while IFS= read -r header; do
  while IFS= read -r type; do
    [[ -z "$type" ]] && continue
    if has_out_of_class_implementation "$type"; then
      continue
    fi
    if is_allowed_header_only_class "$type"; then
      continue
    fi
    add_failure "class $type declared in $header has no out-of-class implementation; mark it header-only or remove the stale declaration"
  done < <(
    grep -E '^[[:space:]]*class[[:space:]]+[A-Z][A-Za-z0-9_]+' "$header" 2>/dev/null \
      | grep -Ev ';[[:space:]]*$' \
      | sed -E 's/^[[:space:]]*class[[:space:]]+([A-Z][A-Za-z0-9_]+).*/\1/'
  )
done < <(find "$LAMBDA_INCLUDE_DIR" -name '*.hpp' -type f | sort)

while IFS= read -r line; do
  type="$(printf '%s\n' "$line" | sed -E 's/.*class[[:space:]]+([A-Z][A-Za-z0-9_]+)[[:space:]]*;.*/\1/')"
  [[ -z "$type" ]] && continue
  if ! has_definition "$type"; then
    if is_allowed_forward_without_definition "$type"; then
      continue
    fi
    add_failure "forward declaration has no matching definition: $line"
  fi
done < <(grep -RnE '^[[:space:]]*class[[:space:]]+[A-Z][A-Za-z0-9_]+[[:space:]]*;' "$LAMBDA_INCLUDE_DIR" "$LAMBDA_SOURCE_DIR" 2>/dev/null)

if ((${#failures[@]} > 0)); then
  printf 'Stale symbol scan failed:\n' >&2
  for failure in "${failures[@]}"; do
    printf '  - %s\n' "$failure" >&2
  done
  exit 1
fi

header_count="$(find "$LAMBDA_INCLUDE_DIR" -type f -name '*.hpp' | wc -l | tr -d '[:space:]')"
source_count="$(find "$LAMBDA_SOURCE_DIR" -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.mm' \) | wc -l | tr -d '[:space:]')"
test_count="$(find "$LAMBDA_TESTS_DIR" -type f \( -name '*.cpp' -o -name '*.mm' -o -name '*.hpp' \) | wc -l | tr -d '[:space:]')"
printf 'Stale symbol scan passed (%d headers, %d source files, %d test files inspected).\n' \
  "$header_count" "$source_count" "$test_count"

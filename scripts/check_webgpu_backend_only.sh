#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

failures=0

report_failure() {
  printf '%s\n' "$1" >&2
  failures=$((failures + 1))
}

for path in \
  include/Lambda/Graphics/WebGpuContext.hpp \
  src/Graphics/Cairo \
  src/Graphics/Metal \
  src/Graphics/OpenGL \
  src/Graphics/Platform \
  src/Graphics/Skia \
  src/Graphics/Vulkan \
  vendor/vma \
  vendor/vma/vk_mem_alloc.h
do
  if [[ -e "$path" ]]; then
    report_failure "legacy rendering backend artifact still exists: $path"
  fi
done

legacy_name_violations="$(
  find include src vendor -type f \
    \( -name '*CairoCanvas*' \
       -o -name '*MetalCanvas*' \
       -o -name '*NativeRenderer*' \
       -o -name '*NativeSurface*' \
       -o -name '*OpenGLCanvas*' \
       -o -name '*RendererBackend*' \
       -o -name '*RenderBackend*' \
       -o -name '*SkiaCanvas*' \
       -o -name '*VulkanCanvas*' \
       -o -name '*VulkanGpu*' \
       -o -name '*vk_mem_alloc*' \) \
    -print
)"
if [[ -n "$legacy_name_violations" ]]; then
  report_failure "legacy rendering backend file names found:"
  printf '%s\n' "$legacy_name_violations" >&2
fi

legacy_symbol_violations="$(
  grep -RInE \
    'WebGpuCanvasHandles|webGpuCanvasHandles|NativeRenderer|NativeSurface|RendererBackend|RenderBackend|CairoCanvas|MetalCanvas|OpenGLCanvas|SkiaCanvas|VulkanCanvas|VulkanGpu|vk_mem_alloc|Vma[A-Z]|VMA_' \
    include src demos tests README.md docs 2>/dev/null || true
)"
if [[ -n "$legacy_symbol_violations" ]]; then
  report_failure "legacy rendering backend symbols found:"
  printf '%s\n' "$legacy_symbol_violations" >&2
fi

if ((failures > 0)); then
  exit 1
fi

printf 'WebGPU backend scan passed.\n'

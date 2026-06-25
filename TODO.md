# Vulkan Backend Decomposition TODO

This document is for a follow-up agent running on Linux with Vulkan development
dependencies installed. It captures the remaining Vulkan work from the architecture
remediation spec after the macOS/Metal-safe stages were completed.

## Current State

Already shipped:

- P4 stage 1: `VulkanGlyphAtlas.inc` was replaced by `src/Graphics/Vulkan/VulkanGlyphAtlas.hpp` and `src/Graphics/Vulkan/VulkanGlyphAtlas.cpp`.
- P4 stage 2: `VulkanSwapchain.inc` was replaced by `src/Graphics/Vulkan/VulkanSwapchain.hpp` and `src/Graphics/Vulkan/VulkanSwapchain.cpp`.
- P4 stage 3: `VulkanPipelines.inc` was replaced by `src/Graphics/Vulkan/VulkanPipelines.hpp` and `src/Graphics/Vulkan/VulkanPipelines.cpp`.
- Shared Vulkan structs needed by the extracted classes live in `src/Graphics/Vulkan/VulkanCanvasShared.hpp`.

The remaining include-concatenation files are:

- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasApi.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasDrawOps.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasLifecycle.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasMembers.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasResources.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCommandRecording.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCore.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanImages.inc`

`src/Graphics/Vulkan/VulkanGpuCanvas.cpp` still assembles `VulkanCanvas` by including the remaining `.inc` files. The final Vulkan target is for that pattern to be gone.

## Global Rules From The Spec

- No behavior change. P4 is a pure refactor; pixel output and test assertions must remain identical.
- Do not weaken `tests/VulkanRenderTargetTests.cpp`.
- Do not invent new cross-backend abstractions here. Mirror the existing Metal decomposition where useful.
- Each stage must compile and pass tests on its own.
- Land each stage as its own commit.
- If a stage cannot be completed safely, stop at the last clean completed stage and document exactly what remains and why.
- Do not leave a half-migrated stage.
- Delete each consumed `.inc` file in the commit that replaces it.
- Add every new `.cpp` to both Linux source lists in `src/CMakeLists.txt`.
- Commit messages and code comments that cite code must use `path:line` references.

## First Linux Task

Before starting new extraction work, build the current tree on Linux and fix any compile-only issues from the already-shipped P4 stages.

Recommended Wayland build:

```sh
cmake -S . -B build-linux-wayland \
  -DLAMBDAUI_PLATFORM=LINUX_WAYLAND \
  -DLAMBDAUI_BUILD_DEMOS=ON \
  -DLAMBDAUI_BUILD_TESTS=ON
cmake --build build-linux-wayland
ctest --test-dir build-linux-wayland --output-on-failure
```

If KMS dependencies are available, also run:

```sh
cmake -S . -B build-linux-kms \
  -DLAMBDAUI_PLATFORM=LINUX_KMS \
  -DLAMBDAUI_BUILD_DEMOS=ON \
  -DLAMBDAUI_BUILD_TESTS=ON
cmake --build build-linux-kms
ctest --test-dir build-linux-kms --output-on-failure
```

If one Linux backend cannot be built because system packages are missing, state that explicitly in the commit or handoff and run every buildable backend.

## Remaining P4 Stages

### Stage 4: Images

Target:

- Replace `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanImages.inc` with real separately compiled image code.
- Create `src/Graphics/Vulkan/VulkanImage.hpp` and `src/Graphics/Vulkan/VulkanImage.cpp`, matching `src/Graphics/Metal/MetalImage.hpp` / `.mm` in role.

Important current responsibilities to preserve:

- The `VulkanImage` implementation currently lives in `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCore.inc`.
- Texture upload, update, transition, readback, descriptor, and image-texture cache helpers currently live in `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanImages.inc`.
- Image factory/API code in `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasApi.inc` must keep the same behavior.
- Owned/external image lifetime and deferred destruction must remain safe across frames.
- DMABUF/imported image paths must keep the same ownership and cleanup behavior.

Suggested shape:

- Move `VulkanImage` into `VulkanImage.hpp` / `.cpp`.
- Move image-specific Vulkan resource management behind a narrow canvas-owned helper if needed.
- Keep canvas-facing methods small, forwarding existing state into the helper.
- Delete `VulkanImages.inc`.
- Add `Graphics/Vulkan/VulkanImage.cpp` to both Linux source lists in `src/CMakeLists.txt`.

Acceptance for this stage:

- `src/Graphics/Vulkan/VulkanGpuCanvas.cpp` no longer includes `VulkanImages.inc`.
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanImages.inc` is deleted.
- `tests/VulkanRenderTargetTests.cpp` passes with identical assertions.
- Full Linux `ctest` passes.

### Stage 5: Frame Recording And Command Recording

Target:

- Replace `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCommandRecording.inc`.
- Consolidate command-recording behavior with `src/Graphics/Vulkan/VulkanFrameRecorder.cpp` / `.hpp` into a real recorder type matching `src/Graphics/Metal/MetalFrameRecorder.hpp` / `.mm` in role.

Important current responsibilities to preserve:

- P2 already promoted cross-backend capture/replay onto `Canvas` and `RecordedOps`.
- Call sites must continue using the abstract `RecordedOps` surface where applicable.
- Backend-specific recorder resources must still be cleaned up through the Vulkan deferred-resource path.
- Prepared recorded ops, local replay, glyph atlas generation checks, descriptor ownership, and replay resource retirement must keep their semantics.

Suggested shape:

- Put command recording code in normal `.hpp` / `.cpp` files owned by the Vulkan recorder or a recorder-adjacent helper.
- Keep any unavoidable Vulkan-specific recorder handle downcast at the backend boundary only.
- Delete `VulkanCommandRecording.inc`.
- Add new `.cpp` files to both Linux source lists in `src/CMakeLists.txt`.

Acceptance for this stage:

- `src/Graphics/Vulkan/VulkanGpuCanvas.cpp` no longer includes `VulkanCommandRecording.inc`.
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCommandRecording.inc` is deleted.
- `tests/VulkanRenderTargetTests.cpp` passes with identical assertions.
- Full Linux `ctest` passes.

### Stage 6: Draw Ops, Lifecycle, Core, Resources, API, Members

Target:

- Remove the remaining `.inc` assembly pattern entirely.
- Collapse the residual split into normal `VulkanCanvas` class code and narrow helpers.

Remaining files to consume:

- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasApi.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasDrawOps.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasLifecycle.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasMembers.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCanvasResources.inc`
- `src/Graphics/Vulkan/VulkanGpuCanvasParts/VulkanCore.inc`

Spec direction:

- Collapse `Members` / `Api` / `Core` into the canvas class proper.
- Keep extracted subobjects owned by `VulkanCanvas` through real members or `std::unique_ptr`.
- Convert former shared `.inc` state into private members on the right owner with narrow interfaces.
- Do not invent a new renderer architecture while doing this.

Acceptance for this stage:

- No `.inc` files remain under `src/Graphics/Vulkan/VulkanGpuCanvasParts/`.
- `src/Graphics/Vulkan/VulkanGpuCanvas.cpp` no longer includes `VulkanGpuCanvasParts/*.inc`.
- `tests/VulkanRenderTargetTests.cpp` passes with identical assertions.
- Full Linux `ctest` passes.

## Final Vulkan Acceptance Checks

Run these from the repo root:

```sh
find src/Graphics/Vulkan/VulkanGpuCanvasParts -name '*.inc' -print
rg -n '#include "Graphics/Vulkan/VulkanGpuCanvasParts/.*\.inc"' src/Graphics/Vulkan
rg -n 'Vulkan(GlyphAtlas|Swapchain|Pipelines|Images|CommandRecording)\.inc' src/Graphics/Vulkan
rg -n 'beginRecordedOpsCaptureForCanvas|endRecordedOpsCaptureForCanvas|replayRecordedOpsForCanvas|replayRecordedLocalOpsForCanvas|requestNextFrameCaptureForCanvas|takeCapturedFrameForCanvas|prepareRecordedOpsForCanvas|recordedOpsGlyphAtlasCurrentForCanvas|dpiScaleForCanvas' include src tests
git diff -- src/UI include/Lambda/UI/Element.hpp | rg '^\+.*std::function'
scripts/check_module_dependencies.sh
scripts/check_stale_symbols.sh
```

Expected results:

- The first three `.inc` checks return nothing in the final state.
- The promoted canvas wrapper grep returns nothing.
- The `std::function` diff check returns nothing.
- Both guard scripts exit 0 and print real scan counts.

Final build/test:

```sh
cmake -S . -B build-linux-wayland \
  -DLAMBDAUI_PLATFORM=LINUX_WAYLAND \
  -DLAMBDAUI_BUILD_DEMOS=ON \
  -DLAMBDAUI_BUILD_TESTS=ON
cmake --build build-linux-wayland
ctest --test-dir build-linux-wayland --output-on-failure
```

Also run the KMS variant if available:

```sh
cmake -S . -B build-linux-kms \
  -DLAMBDAUI_PLATFORM=LINUX_KMS \
  -DLAMBDAUI_BUILD_DEMOS=ON \
  -DLAMBDAUI_BUILD_TESTS=ON
cmake --build build-linux-kms
ctest --test-dir build-linux-kms --output-on-failure
```

## Handoff Notes To Include

When handing back the Linux work, state:

- Which P4 stages shipped.
- Which Linux backend(s) were compiled: Wayland, KMS, or both.
- Whether `tests/VulkanRenderTargetTests.cpp` passed unchanged.
- Whether any Vulkan output assertions changed. They should not.
- Any deferred work, with exact remaining file names and reason.

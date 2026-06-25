# Architecture Follow-up TODO

This document tracks remaining architecture-remediation spec items after
`9c9f244` (`Complete Vulkan backend decomposition`) and the follow-up stack
layout deduplication work. Keep it updated whenever one of these items is
implemented or intentionally deferred again.

## Current Status

- P0 layout dedup: completed after the follow-up refactor. Stack mount and
  relayout now share the same post-measurement slot/container geometry path in
  `src/UI/MountContext.cpp`, including collapsed-but-mounted child placement.
- P1 reflow: completed. Reactive text changes reflow the nearest layout owner.
- P2 canvas capture: completed. Capture/replay is exposed through `Canvas` and
  `RecordedOps` instead of promoted backend helpers.
- P3 guard scripts: completed. Guard scripts scan the repository paths and fail
  on missing scan roots.
- P4 Vulkan decomposition: accepted by the original "no `.inc` files remain"
  criterion, but see the optional Vulkan follow-up below.
- P5 reactive atomics: completed. Profiling counters are gated by
  `LAMBDAUI_PROFILE_REACTIVE`.
- P6, P7, P8: deferred.

## P4 Optional Vulkan Core Split

Status: Deferred by scope decision.

The Vulkan include-concatenation pattern is gone, and
`src/Graphics/Vulkan/VulkanGpuCanvasParts/` no longer contains `.inc` files.
`src/Graphics/Vulkan/VulkanGpuCanvas.cpp` is still intentionally large because
the remaining draw-operation, lifecycle, and resource-retirement code is highly
coupled.

Implement this only if true Metal-style file-size parity is required:

- Split cohesive draw-operation code out of `src/Graphics/Vulkan/VulkanGpuCanvas.cpp`
  without introducing a new renderer architecture.
- Keep `VulkanCanvas` as the owner of backend state unless a narrow helper has a
  clear ownership boundary.
- Preserve all Vulkan output assertions and do not weaken
  `tests/VulkanRenderTargetTests.cpp`.
- Add every new Vulkan `.cpp` file to both Linux Vulkan source lists in
  `src/CMakeLists.txt`.
- Validate on Linux Wayland, and KMS when dependencies are available.

Suggested acceptance checks:

```sh
find src/Graphics/Vulkan/VulkanGpuCanvasParts -name '*.inc' -print
rg -n '#include "Graphics/Vulkan/VulkanGpuCanvasParts/.*\.inc"' src/Graphics/Vulkan
ctest --test-dir build-linux-wayland --output-on-failure
```

## P6 UI `std::function` Storage Cleanup

Status: Deferred.

Spec intent: remove the remaining hot-path `std::function` storage in `src/UI`
and the `Element` handler/modifier path. The review counted roughly 140
remaining `std::function` uses under `src/UI`; this item was optional but should
not disappear from tracking.

Current known hotspots:

- `include/Lambda/UI/Element.hpp`: public event handler overloads still accept
  `std::function`.
- `include/Lambda/UI/Detail/ElementModifiers.hpp`: handler storage still uses
  `std::function`.
- `src/UI/Element/ElementModifiers.cpp`: modifier builders still move handlers
  into `std::function` storage.
- `src/UI/Application.cpp` and `src/UI/Application.mm`: menu, frame, and event
  callback maps still use `std::function`.
- Higher-level view helpers in `src/UI/Views/` still construct or store
  `std::function` handlers.

Implementation direction:

- Prefer the repository's existing small-callable type where it fits the
  lifetime and copyability requirements.
- Keep source compatibility for public view APIs unless the spec explicitly
  allows a breaking API change.
- Convert internal storage first, especially the `Element` modifier path.
- Avoid replacing clear public API boundaries with bespoke templates unless it
  measurably reduces allocations and does not bloat call sites.

Acceptance:

```sh
rg -n 'std::function' src/UI include/Lambda/UI
```

The result should be zero for the original target, or every remaining match must
be documented as an intentional public API or platform integration exception.

## P7 `Element` Allocation Consolidation

Status: Deferred.

Spec intent: reduce the separate heap allocations carried by `Element`.

Current allocation-bearing members in `include/Lambda/UI/Element.hpp`:

- `std::vector<std::shared_ptr<detail::EnvironmentOverride const>> envOverrides_`
- `std::shared_ptr<detail::ElementModifiers> modifiers_`
- `std::unique_ptr<detail::LayoutOverrides> overrides_`

Implementation direction:

- Replace small environment override storage with the existing
  `Lambda/Detail/SmallVector.hpp` where practical.
- Investigate combining modifier/layout override storage so common `Element`
  values do not pay three independent allocations.
- Preserve copy/move behavior and the current type-erased component ownership
  semantics.
- Add tests around copying elements with environment overrides, layout overrides,
  keys, and event modifiers before changing storage.

Acceptance:

- Existing UI, mount, and modifier tests pass.
- Copying an `Element` with modifiers, environment overrides, and layout
  overrides preserves behavior.
- The final shape and any remaining intentional allocations are documented here.

## P8 Platform Frame/Event Pump Boundary

Status: Deferred.

Spec intent: move frame-pump and event-poll responsibilities off the generic
`platform::Window` surface.

Current generic methods in `src/UI/Platform/Window.hpp`:

- `eventFd()`
- `wakeFd()`
- `waitForEvents(int timeoutMs)`
- `requestAnimationFrame()`

Known platform implementations/call sites:

- `src/UI/Application.cpp`
- `src/UI/Application.mm`
- `src/Platform/Linux/WaylandWindow.cpp`
- `src/Platform/Linux/KmsWindow.cpp`
- `src/Platform/Mac/MacMetalWindow.mm`

Implementation direction:

- Introduce a narrower frame/event pump interface owned by the application or
  platform layer, not by every generic window.
- Keep platform-specific readiness details in Linux and macOS implementations.
- Preserve the behavior needed by animation frame scheduling and external event
  poll sources.
- Update both Linux backends and the macOS path in the same stage if the generic
  interface changes.

Acceptance:

- `platform::Window` no longer exposes event file descriptors, blocking waits,
  or frame scheduling directly.
- Animation demos continue ticking without requiring a resize.
- Linux Wayland/KMS event integration and macOS event waiting still pass their
  existing tests or manual smoke checks.

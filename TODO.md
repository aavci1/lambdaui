# Architecture Follow-up Status

All architecture-remediation follow-up items tracked after `9c9f244`
(`Complete Vulkan backend decomposition`) have been implemented in this branch.

## Completed Items

- P0 layout dedup: stack mount and relayout share the same post-measurement
  slot/container geometry path in `src/UI/MountContext.cpp`, including
  collapsed-but-mounted child placement.
- P1 reflow: reactive text changes reflow the nearest layout owner.
- P2 canvas capture: capture/replay is exposed through `Canvas` and
  `RecordedOps` instead of promoted backend helpers.
- P3 guard scripts: guard scripts scan repository paths and fail on missing scan
  roots.
- P4 Vulkan decomposition: `.inc` concatenation is gone, subsystem TUs are
  split out, and draw command range recording now lives in
  `src/Graphics/Vulkan/VulkanDrawCommands.cpp`.
- P5 reactive atomics: profiling counters are gated by
  `LAMBDAUI_PROFILE_REACTIVE`.
- P6 UI callback storage: `std::function` usage has been removed from `src/UI`
  and `include/Lambda/UI`; UI callbacks now use `Reactive::SmallFn`.
- P7 `Element` allocation consolidation: environment overrides use inline
  `SmallVector` storage, and modifier/layout override state shares one
  copy-on-write `ElementOptions` allocation.
- P8 platform frame/event pump boundary: frame wait, event fd, wake, and frame
  scheduling methods moved off `platform::Window` onto
  `platform::WindowEventPump`.

## Acceptance Checks

These checks were run and passed on the current macOS checkout after the
architecture follow-up implementation:

```sh
rg -n 'std::function' src/UI include/Lambda/UI
rg -n 'virtual void waitForEvents|virtual int eventFd|virtual int wakeFd|virtual void requestAnimationFrame' src/UI/Platform/Window.hpp
find src/Graphics/Vulkan/VulkanGpuCanvasParts -name '*.inc' -print
rg -n '#include "Graphics/Vulkan/VulkanGpuCanvasParts/.*\.inc"' src/Graphics/Vulkan
scripts/check_module_dependencies.sh
scripts/check_stale_symbols.sh
ctest --test-dir build --output-on-failure
```

Full Linux Vulkan validation still needs to be run on a Linux host with Vulkan,
Wayland/KMS, libdrm, xkbcommon, HarfBuzz, FreeType, and glslang dependencies
installed. That validation should include a clean configure/build plus
`ctest --test-dir build --output-on-failure` for the Linux backend targets.

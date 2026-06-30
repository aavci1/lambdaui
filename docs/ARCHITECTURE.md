# Architecture

LambdaUI is organized as a native C++ UI framework with a retained scene graph, fine-grained reactivity, backend-specific renderers, and platform window/input integrations.

## Repository Layout

```text
include/Lambda/       Public API headers
src/                  Framework implementation
demos/                Standalone LambdaUI applications
tests/                doctest test suite
bench/                Optional benchmarks
cmake/                CMake app, resource, and protocol helpers
resources/            Runtime resources copied into apps
protocols/            Wayland protocol XML and local stubs
tools/                Asset-generation utilities
vendor/               Small vendored headers
docs/                 Current project documentation
```

Use `include/Lambda.hpp` as the broad umbrella include. Use narrower headers from `include/Lambda/...` for production code that wants lower compile time.

## Public Modules

- `Core`: geometry, color, identity, and small foundational types.
- `Reactive`: `Signal`, `Computed`, `Effect`, `Scope`, bindables, animation clocks, transitions, and tracking utilities.
- `Graphics`: canvas abstraction, styles, text layout, images, SVG paths, render targets, and backend-facing drawing types.
- `SceneGraph`: retained nodes, layout constraints, hit testing, interaction metadata, traversal, and scene rendering.
- `Layout`: layout primitives and stack algorithms.
- `UI`: `Application`, `Window`, `Element`, components, hooks, environments, commands, menus, overlays, popovers, and built-in views.
- `Debug`: debug flags and performance counters.

The implementation mirrors these modules in `src/`. Platform-specific code lives under `src/Platform`, `src/Graphics/Metal`, `src/Graphics/Vulkan`, and `src/Graphics/Linux`.

## Runtime Flow

The high-level flow for a typical app is:

1. `Application` creates the platform application, event queue, text system, and windows.
2. A `Window` receives a root component through `Window::setView`.
3. The root component is mounted into a retained `scenegraph::SceneGraph`.
4. Component `body()` functions declare the initial element tree for a mount cycle.
5. Hooks and control-flow views create scope-owned reactive state, effects, rows, and branches.
6. Bindable values install effects that update retained scene nodes after mount.
7. Platform input is normalized into LambdaUI input events and routed by `Runtime`.
8. Dirty retained nodes request redraws.
9. `SceneRenderer` traverses the scene graph and emits drawing commands to `Canvas`.
10. The active backend presents through Metal or Vulkan.

The key point: ordinary component bodies are mount-time declarations. LambdaUI does not repeatedly rerun arbitrary bodies to discover every property change. Reactive bindings and control-flow views update the retained tree.

## Application And Window Layer

`Application` owns process-level services:

- The main event loop.
- The `EventQueue`.
- Timers and event poll sources.
- The text system.
- Menus and command dispatch.
- Window ownership and persisted window state.
- Platform application integration.

`Window` owns window-level state:

- Window size, title, fullscreen state, chrome mode, background, and theme.
- A lazily created `Canvas`.
- A lazily created retained scene graph.
- Root view mounting through `setView`.
- Popovers, popup menus, overlays, redraw requests, and cursor updates.

See `include/Lambda/UI/Application.hpp`, `include/Lambda/UI/Window.hpp`, and `include/Lambda/UI/WindowUI.hpp`.

## UI Layer

The UI layer is built around `Element`, component bodies, and views:

- Components usually expose `auto body() const` or `Element body() const`.
- Views can also implement `measure(...)` and `mount(...)` when they need custom layout or retained node construction.
- `MountContext` owns the active reactive scope, environment binding, runtime, and mount metadata.
- `MeasureContext` carries measurement constraints and environment data during layout.
- View modifiers attach layout, visual, and interaction behavior to elements.

Built-in views live under `include/Lambda/UI/Views` and `src/UI/Views`.

## Reactivity And Ownership

Reactive state is scoped. Mounting creates owner scopes; component mounts and control-flow rows/branches create child scopes. Disposing a scope runs cleanup callbacks and disposes owned effects.

The main primitives are:

- `Signal<T>` for mutable state.
- `Computed<T>` for derived values.
- `Effect` for side effects.
- `Scope` for lifetime.
- `Bindable<T>` for values that may be constants or reactive closures.

Hooks such as `useState`, `useComputed`, `useEffect`, `useAnimated`, and `useEnvironment` require an active owner scope. See [Reactivity](REACTIVITY.md) and [UI Model](UI_MODEL.md).

## Scene Graph

The scene graph is the retained identity layer. It contains nodes for groups, rectangles, text, images, paths, custom render nodes, and raster-cache nodes.

Important responsibilities:

- Store local bounds, transforms, layout flow, opacity, clipping, and interaction data.
- Track dirty and subtree-dirty state.
- Preserve node identity across retained updates.
- Support hit testing and traversal.
- Hold prepared render operation caches where possible.

Core files:

- `include/Lambda/SceneGraph/SceneNode.hpp`
- `include/Lambda/SceneGraph/SceneGraph.hpp`
- `include/Lambda/SceneGraph/SceneTraversal.hpp`
- `src/SceneGraph/SceneRenderer.cpp`

## Rendering

Rendering flows through the backend-neutral `Canvas` interface. The scene renderer traverses retained nodes and emits draw operations for fills, strokes, images, text, paths, clips, opacity, transforms, backdrop effects, and cached subtrees.

Renderer selection is split from platform selection. `LAMBDAUI_RENDERER=AUTO` prefers Dawn/WebGPU when Dawn is explicitly configured or discoverable, then falls back to the existing native renderer while the port is completed. `LAMBDAUI_ENABLE_NATIVE_RENDERERS=OFF` disables that fallback and turns missing Dawn into a configure-time error. `LAMBDAUI_RENDERER=NATIVE` keeps the existing Metal or Vulkan renderer, while `LAMBDAUI_RENDERER=WEBGPU` requires Dawn and routes window surfaces through WebGPU.

macOS rendering:

- Cocoa windowing.
- Metal canvas and render targets.
- CoreText text system.
- Metal shaders compiled and embedded at build time.

Linux rendering:

- Vulkan canvas and render targets.
- FreeType/fontconfig/HarfBuzz text system.
- GLSL shaders compiled to SPIR-V headers at build time.
- Wayland client presentation.

WebGPU rendering:

- Dawn `webgpu.h` device/surface setup.
- Dawn is discovered as an installed CMake package, added from `LAMBDAUI_DAWN_SOURCE_DIR`, or fetched with `LAMBDAUI_DAWN_FETCH`.
- CAMetalLayer surfaces on macOS and Wayland surfaces on Linux.
- WGSL pipelines for primitives, paths, images, glyphs, clips, render targets, and frame readback.

Rendering code is split across:

- `src/Graphics/Metal`
- `src/Graphics/Vulkan`
- `src/Graphics/WebGPU`
- `src/Graphics/Linux`
- `src/SceneGraph/SceneRenderer.cpp`

## Input And Interaction

Platform input is normalized into LambdaUI input events, then routed by `Runtime`.

Key ideas:

- Pointer hit testing uses reverse-order scene traversal.
- Press targets capture pointer-up and drag-related pointer-move handling.
- Focus, hover, press, and keyboard focus are exposed through hook-backed signals.
- Scroll events route to the nearest scroll-capable target.
- Keyboard events route through focus, command/menu shortcut handling, and target handlers.
- Window drag and resize regions can be represented in retained interaction metadata.

Useful files:

- `include/Lambda/UI/Input.hpp`
- `include/Lambda/UI/Events.hpp`
- `include/Lambda/UI/InteractionData.hpp`
- `src/UI/Runtime.cpp`
- `include/Lambda/SceneGraph/SceneTraversal.hpp`

## Event Queue

`EventQueue` is the application-visible dispatch path for lifecycle, window, input, timer, and custom events. It has a fixed bucket order and drains pending events until all buckets are empty. See [Event Queue](EVENTS.md).

## Platform Backends

Platform selection is controlled by `LAMBDAUI_PLATFORM`; renderer selection is controlled by `LAMBDAUI_RENDERER`.

`MACOS`:

- Uses Objective-C++ sources in `src/Platform/Mac`.
- Uses Cocoa, Metal, MetalKit, QuartzCore, Foundation, CoreText, and CoreVideo.
- Defines `LAMBDAUI_METAL=1`.

`LINUX_WAYLAND`:

- Uses Wayland client protocols and `src/Platform/Linux/Wayland*`.
- Uses Vulkan for rendering.
- Supports xdg-shell and several optional protocols for scaling, layer-shell, background effects, pointer constraints, activation, and related behavior.
- Defines `LAMBDAUI_VULKAN=1`.

`WEBGPU` renderer:

- Uses Dawn for GPU device, queue, surface, and WebGPU command encoding.
- Defines `LAMBDAUI_WEBGPU=1`.

## Runtime Resources

`resources/fonts/MaterialSymbolsRounded.ttf` is copied next to app executables by `lambdaui_add_app`. Icons and icon names are generated from that bundled font through the tools under `tools/`.

## Where To Change Things

- Add or change a built-in control: `include/Lambda/UI/Views`, `src/UI/Views`, and a matching demo/test when practical.
- Change reactive behavior: `include/Lambda/Reactive` and `src/Reactive`.
- Change layout behavior: `include/Lambda/Layout`, `src/Layout`, `SceneNode::relayout`, and layout tests.
- Change rendering output: `src/SceneGraph/SceneRenderer.cpp` plus the relevant Metal/Vulkan canvas code.
- Change input routing: `src/UI/Runtime.cpp`, `include/Lambda/SceneGraph/SceneTraversal.hpp`, and runtime input tests.
- Change platform window behavior: `src/Platform/Mac` or `src/Platform/Linux/Wayland*`.
- Change app build integration: `cmake/LambdaApp.cmake`.

When in doubt, start with the public header in `include/Lambda/...`, then follow the matching implementation in `src/...`.

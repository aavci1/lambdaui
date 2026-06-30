# LambdaUI

LambdaUI is a C++23 UI framework for building native desktop interfaces with declarative component bodies, fine-grained reactivity, and a retained scene graph. It builds as a shared CMake library and renders through Dawn/WebGPU on macOS and Linux.

The public API lives under `include/Lambda`, implementation lives under `src`, and examples live under `demos`.

## Highlights

- C++23 component model with `Element body() const` roots.
- Scope-owned hooks such as `useState`, `useComputed`, `useEffect`, `useAnimated`, `useFrame`, and `useEnvironment`.
- Retained scene graph with reactive bindings, keyed control-flow views, hit testing, layout, and render caching.
- Built-in views for text, stacks, grids, scroll views, buttons, text input, menus, popovers, dialogs, alerts, sliders, tables, icons, SVG, and more.
- Dawn/WebGPU renderer for macOS CAMetalLayer and Linux Wayland surfaces.
- CMake helpers for apps and demos, plus doctest-based unit/integration tests.

## Quick Start

Prerequisites:

- CMake 3.25 or newer.
- A C++23-capable compiler.
- WebGPU renderer: Dawn installed with CMake package files, a local Dawn source checkout, or `LAMBDAUI_DAWN_FETCH=ON`.
- Linux Wayland: development packages for Wayland, wayland-protocols, xkbcommon, FreeType, fontconfig, HarfBuzz, librsvg, zlib, and pkg-config.

Build the library, demos, and tests:

```sh
cmake -S . -B build -DLAMBDAUI_DAWN_FETCH=ON -DLAMBDAUI_BUILD_DEMOS=ON -DLAMBDAUI_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

Run a demo:

```sh
./build/demos/hello-world
```

Choose a backend explicitly when needed:

```sh
cmake -S . -B build-macos-webgpu -DLAMBDAUI_PLATFORM=MACOS -DLAMBDAUI_DAWN_FETCH=ON
cmake -S . -B build-linux-webgpu -DLAMBDAUI_PLATFORM=LINUX_WAYLAND -DLAMBDAUI_DAWN_FETCH=ON
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=AUTO -DCMAKE_PREFIX_PATH=/path/to/dawn/install
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=WEBGPU -DCMAKE_PREFIX_PATH=/path/to/dawn/install
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=WEBGPU -DLAMBDAUI_DAWN_SOURCE_DIR=/path/to/dawn
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=WEBGPU -DLAMBDAUI_DAWN_FETCH=ON
```

`LAMBDAUI_PLATFORM=AUTO` is the default. It selects `MACOS` on Apple hosts and `LINUX_WAYLAND` on Linux/Unix hosts.
`LAMBDAUI_RENDERER=AUTO` is the default. It selects `WEBGPU` when Dawn is explicitly configured or discoverable through `CMAKE_PREFIX_PATH`; otherwise configure fails with Dawn setup guidance.

## Minimal App

```cpp
#include <Lambda.hpp>
#include <Lambda/UI/UI.hpp>
#include <Lambda/UI/Views/Views.hpp>

using namespace lambdaui;

struct HelloRoot {
  auto body() const {
    return Text{
        .text = "Hello, World!",
        .font = Font::largeTitle(),
        .color = Color::primary(),
        .horizontalAlignment = HorizontalAlignment::Center,
        .verticalAlignment = VerticalAlignment::Center,
    };
  }
};

int main(int argc, char* argv[]) {
  Application app(argc, argv);

  auto& window = app.createWindow<Window>({
      .size = {400, 400},
      .title = "Hello, World!",
  });
  window.setView<HelloRoot>();

  return app.exec();
}
```

See `demos/hello-world/main.cpp` and the other demos for complete examples.

## Documentation

- [Getting Started](docs/GETTING_STARTED.md): dependencies, build options, demos, tests, and app integration.
- [Architecture](docs/ARCHITECTURE.md): repository map, module boundaries, runtime flow, rendering, input, and platform backends.
- [UI Model](docs/UI_MODEL.md): components, retained mounting, hooks, built-in views, bindings, environments, and control flow.
- [Reactivity](docs/REACTIVITY.md): signals, computed values, effects, scopes, bindables, and animation helpers.
- [Event Queue](docs/EVENTS.md): event types, dispatch order, threading expectations, timers, and custom payloads.
- [Style Guide](docs/STYLE_GUIDE.md): source layout, component body formatting, module conventions, tests, and docs conventions.

## Repository Map

```text
include/Lambda/       Public headers
src/                  Framework implementation
demos/                Standalone example apps
tests/                doctest test suite
bench/                Optional benchmarks
cmake/                CMake app/resource/protocol helpers
resources/            Runtime resources copied into apps
protocols/            Wayland protocol XML and local stubs
tools/                Asset-generation utilities
vendor/               Small vendored headers
docs/                 Project documentation
```

## Important CMake Options

- `LAMBDAUI_PLATFORM`: `AUTO`, `MACOS`, or `LINUX_WAYLAND`.
- `LAMBDAUI_RENDERER`: `AUTO` or `WEBGPU`.
- `LAMBDAUI_DAWN_SOURCE_DIR`: optional Dawn source checkout for WebGPU builds.
- `LAMBDAUI_DAWN_FETCH`: fetch Dawn with CMake `FetchContent` for WebGPU builds.
- `LAMBDAUI_DAWN_GIT_REPOSITORY`: Dawn repository used by `LAMBDAUI_DAWN_FETCH`.
- `LAMBDAUI_DAWN_GIT_TAG`: Dawn tag, branch, or commit used by `LAMBDAUI_DAWN_FETCH`.
- `LAMBDAUI_BUILD_DEMOS`: build `demos/`.
- `LAMBDAUI_BUILD_TESTS`: build `lambda-tests` and register it with CTest.
- `LAMBDAUI_BUILD_BENCHMARKS`: build optional benchmarks.
- `LAMBDAUI_ENABLE_ASAN`: build with AddressSanitizer.
- `LAMBDAUI_ENABLE_WARNINGS`: enable common compiler warnings.
- `LAMBDAUI_PROFILE_REACTIVE`: compile reactive profiling counters.
- `LAMBDAUI_ENABLE_DEFAULT_EVENT_LOGGING`: log built-in application event handlers.

The CMake target exports `LAMBDAUI_WEBGPU=1` for supported renderer builds.
For WebGPU builds, `webGpuCanvasHandles(canvas)` returns borrowed Dawn handles, and `WebGpuRenderTargetSpec` can render into an internal target or a caller-owned `WGPUTextureView`.

The build uses CMake `FetchContent` for `libtess2`, and for `doctest` when tests are enabled.

## License

LambdaUI is available under the [MIT license](LICENCE.md).

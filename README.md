# LambdaUI

LambdaUI is a C++23 UI framework for building native desktop interfaces with declarative component bodies, fine-grained reactivity, and a retained scene graph. It builds as a shared CMake library and includes a Dawn/WebGPU renderer that is preferred when Dawn is available, with native macOS/Metal and Linux/Vulkan backends still available during the migration.

The public API lives under `include/Lambda`, implementation lives under `src`, and examples live under `demos`.

## Highlights

- C++23 component model with `Element body() const` roots.
- Scope-owned hooks such as `useState`, `useComputed`, `useEffect`, `useAnimated`, `useFrame`, and `useEnvironment`.
- Retained scene graph with reactive bindings, keyed control-flow views, hit testing, layout, and render caching.
- Built-in views for text, stacks, grids, scroll views, buttons, text input, menus, popovers, dialogs, alerts, sliders, tables, icons, SVG, and more.
- Dawn/WebGPU renderer for macOS CAMetalLayer and Linux Wayland surfaces.
- Metal renderer on macOS with Cocoa/CoreText integration while the native backend remains enabled.
- Vulkan renderer on Linux with a Wayland backend while the native backend remains enabled.
- CMake helpers for apps and demos, plus doctest-based unit/integration tests.

## Quick Start

Prerequisites:

- CMake 3.25 or newer.
- A C++23-capable compiler.
- macOS native renderer: full Xcode with `xcrun`, `metal`, `metallib`, and `xxd`.
- Linux Wayland native renderer: development packages for Wayland, wayland-protocols, libdrm, xkbcommon, Vulkan, FreeType, fontconfig, HarfBuzz, librsvg, zlib, pkg-config, and glslang.
- WebGPU renderer: Dawn installed with CMake package files, or a local Dawn source checkout.

Build the library, demos, and tests:

```sh
cmake -S . -B build -DLAMBDAUI_BUILD_DEMOS=ON -DLAMBDAUI_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

Run a demo:

```sh
./build/demos/hello-world
```

Choose a backend explicitly when needed:

```sh
cmake -S . -B build -DLAMBDAUI_PLATFORM=MACOS
cmake -S . -B build -DLAMBDAUI_PLATFORM=LINUX_WAYLAND
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=AUTO -DCMAKE_PREFIX_PATH=/path/to/dawn/install
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=WEBGPU -DCMAKE_PREFIX_PATH=/path/to/dawn/install
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=WEBGPU -DLAMBDAUI_DAWN_SOURCE_DIR=/path/to/dawn
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=WEBGPU -DLAMBDAUI_DAWN_FETCH=ON
cmake -S . -B build-webgpu-only -DLAMBDAUI_RENDERER=AUTO -DLAMBDAUI_ENABLE_NATIVE_RENDERERS=OFF -DLAMBDAUI_DAWN_FETCH=ON
```

`LAMBDAUI_PLATFORM=AUTO` is the default. It selects `MACOS` on Apple hosts and `LINUX_WAYLAND` on Linux/Unix hosts.
`LAMBDAUI_RENDERER=AUTO` is the default. It selects `WEBGPU` when Dawn is explicitly configured or discoverable through `CMAKE_PREFIX_PATH`, and falls back to `NATIVE` while the port is completed. Set `LAMBDAUI_ENABLE_NATIVE_RENDERERS=OFF` to enforce the Dawn/WebGPU-only path.

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
- `LAMBDAUI_RENDERER`: `AUTO`, `NATIVE`, or `WEBGPU`.
- `LAMBDAUI_ENABLE_NATIVE_RENDERERS`: allow the legacy Metal/Vulkan renderers as a temporary fallback while WebGPU replaces them.
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

The build uses CMake `FetchContent` for `libtess2`, and for `doctest` when tests are enabled.

## License

LambdaUI is available under the [MIT license](LICENCE.md).

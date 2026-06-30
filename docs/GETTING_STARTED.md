# Getting Started

This guide covers the practical path from a fresh checkout to a running LambdaUI app.

## Requirements

Common requirements:

- CMake 3.25 or newer.
- A C++23-capable compiler.
- Git and network access for CMake `FetchContent` dependencies.

macOS requirements:

- macOS 12 or newer deployment target.
- Xcode or Command Line Tools with a macOS SDK.
- Cocoa, Foundation, QuartzCore, CoreText, and CoreVideo frameworks.

Linux Wayland requirements:

- `pkg-config`.
- `wayland-client`, `wayland-cursor`, and `wayland-protocols`.
- `xkbcommon`, FreeType, fontconfig, HarfBuzz, librsvg, and zlib.

Package names vary by distribution. Install development packages, not just runtime packages.

WebGPU renderer requirements:

- Dawn with the `dawn::webgpu_dawn` CMake target, either installed and discoverable through `CMAKE_PREFIX_PATH`, supplied as a source checkout with `LAMBDAUI_DAWN_SOURCE_DIR`, or fetched by CMake with `LAMBDAUI_DAWN_FETCH=ON`.

## Build

Build the framework only:

```sh
cmake -S . -B build -DLAMBDAUI_DAWN_FETCH=ON
cmake --build build
```

Build demos and tests:

```sh
cmake -S . -B build -DLAMBDAUI_DAWN_FETCH=ON -DLAMBDAUI_BUILD_DEMOS=ON -DLAMBDAUI_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

Build one target:

```sh
cmake --build build --target lambdaui
cmake --build build --target lambda-tests
cmake --build build --target hello-world
```

Run a demo:

```sh
./build/demos/hello-world
```

## Platform Selection

The main platform switch is `LAMBDAUI_PLATFORM`:

```sh
cmake -S . -B build -DLAMBDAUI_PLATFORM=AUTO -DLAMBDAUI_DAWN_FETCH=ON
cmake -S . -B build-macos-webgpu -DLAMBDAUI_PLATFORM=MACOS -DLAMBDAUI_DAWN_FETCH=ON
cmake -S . -B build-linux-webgpu -DLAMBDAUI_PLATFORM=LINUX_WAYLAND -DLAMBDAUI_DAWN_FETCH=ON
```

`AUTO` selects `MACOS` on Apple hosts and `LINUX_WAYLAND` on Linux/Unix hosts.

Rendering always uses Dawn/WebGPU. Configure Dawn with an installed package, a source checkout, or FetchContent:

```sh
cmake -S . -B build-webgpu -DCMAKE_PREFIX_PATH=/path/to/dawn/install
cmake -S . -B build-webgpu -DLAMBDAUI_DAWN_SOURCE_DIR=/path/to/dawn
cmake -S . -B build-webgpu -DLAMBDAUI_DAWN_FETCH=ON
```

Dawn can be discovered through `CMAKE_PREFIX_PATH`, supplied through `LAMBDAUI_DAWN_SOURCE_DIR`, or fetched with `LAMBDAUI_DAWN_FETCH=ON`. If Dawn is not available, configure fails with Dawn setup guidance.

WebGPU APIs are always available. `Canvas::webGpuDevice()`, `Canvas::webGpuQueue()`, and `Canvas::webGpuRenderTargetFormat()` expose borrowed Dawn handles for resource creation. Pass the device and queue together to image upload helpers when eager upload is needed. WebGPU render targets use `WebGpuRenderTargetSpec`; set its `device` and `queue` fields to allocate Lambda's internal texture on an existing Dawn device, or add `textureView` to render into a caller-owned Dawn/WebGPU texture view.

## Useful Build Options

- `LAMBDAUI_BUILD_DEMOS`: build standalone demos under `demos/`.
- `LAMBDAUI_DAWN_SOURCE_DIR`: optional Dawn source checkout for WebGPU builds.
- `LAMBDAUI_DAWN_FETCH`: fetch Dawn with CMake `FetchContent` when an installed package or source checkout is not provided. LambdaUI skips Dawn's git submodules and lets Dawn's dependency fetcher clone the required subset.
- `LAMBDAUI_DAWN_GIT_REPOSITORY`: Dawn repository used by `LAMBDAUI_DAWN_FETCH`.
- `LAMBDAUI_DAWN_GIT_TAG`: Dawn tag, branch, or commit used by `LAMBDAUI_DAWN_FETCH`.
- `LAMBDAUI_BUILD_TESTS`: build `lambda-tests` and register it with CTest.
- `LAMBDAUI_BUILD_BENCHMARKS`: build benchmarks under `bench/`.
- `LAMBDAUI_ENABLE_ASAN`: enable AddressSanitizer for framework, apps, and tests.
- `LAMBDAUI_ENABLE_WARNINGS`: enable common compiler warnings.
- `LAMBDAUI_PROFILE_REACTIVE`: compile deterministic reactive profiling counters.
- `LAMBDAUI_DISABLE_VARIANT_REFS`: force paragraph cache layouts to deep-copy instead of retaining variant references.
- `LAMBDAUI_ENABLE_PARAGRAPH_CACHE_PARALLEL_ASSERT`: compare incremental and full paragraph-cache assembly in debug/test builds.
- `LAMBDAUI_ENABLE_DEFAULT_EVENT_LOGGING`: print built-in application event handlers to stdout.

## App Integration

For apps in the same CMake build, include LambdaUI and use the app helper:

```cmake
add_subdirectory(path/to/lambdaui)

lambdaui_add_app(my-app
  APP_NAME "My App"
  BUNDLE_IDENTIFIER "com.example.my-app"
  SOURCES main.cpp)
```

`lambdaui_add_app` links the app with `lambdaui` and copies bundled runtime resources, including `resources/fonts/MaterialSymbolsRounded.ttf`, next to the executable.

For small examples, include the umbrella headers:

```cpp
#include <Lambda.hpp>
#include <Lambda/UI/UI.hpp>
#include <Lambda/UI/Views/Views.hpp>
```

For larger codebases, prefer specific headers from `include/Lambda/...` where compile time matters.

## Tests

The test suite uses doctest and builds as `lambda-tests` when `LAMBDAUI_BUILD_TESTS=ON`.

```sh
cmake -S . -B build -DLAMBDAUI_BUILD_TESTS=ON
cmake --build build --target lambda-tests
cmake --build build --target lambdaui_static_checks
ctest --test-dir build --output-on-failure
```

Platform-specific tests are included for the selected platform. `lambdaui_static_checks` includes module dependency validation and the WebGPU-only backend guard. The active renderer path is Dawn/WebGPU.

## Demos

Enable demos with `LAMBDAUI_BUILD_DEMOS=ON`. The demos exercise the main built-in controls and are a useful source of copyable application code:

- `hello-world`: smallest app skeleton.
- `layout-demo`: stack, alignment, and layout behavior.
- `theme-demo`: theme tokens, inputs, controls, and scrolling.
- `animation-demo`: reactive animation and render-cache behavior.
- `scene-graph-demo`: direct retained scene-graph use.
- Control demos such as `button-demo`, `textinput-demo`, `table-demo`, `popover-demo`, `toast-demo`, `slider-demo`, and `select-demo`.

## Troubleshooting

If Linux configuration fails, read the missing `pkg-config` package in the CMake error and install the matching development package.

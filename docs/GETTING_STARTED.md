# Getting Started

This guide covers the practical path from a fresh checkout to a running LambdaUI app.

## Requirements

Common requirements:

- CMake 3.25 or newer.
- A C++23-capable compiler.
- Git and network access for CMake `FetchContent` dependencies.

macOS requirements:

- macOS 12 or newer deployment target.
- Full Xcode, not only Command Line Tools.
- `xcrun`, `metal`, `metallib`, and `xxd` available on `PATH`.
- Cocoa, Foundation, QuartzCore, Metal, MetalKit, CoreText, and CoreVideo frameworks.

Linux Wayland requirements:

- `pkg-config`.
- `wayland-client`, `wayland-cursor`, and `wayland-protocols`.
- `xkbcommon`, FreeType, fontconfig, HarfBuzz, librsvg, and zlib.
- Native renderer only: `libdrm`, Vulkan loader/headers, and `glslangValidator` or `glslang` for shader compilation.

Package names vary by distribution. Install development packages, not just runtime packages.

WebGPU renderer requirements:

- Dawn with the `dawn::webgpu_dawn` CMake target, either installed and discoverable through `CMAKE_PREFIX_PATH`, supplied as a source checkout with `LAMBDAUI_DAWN_SOURCE_DIR`, or fetched by CMake with `LAMBDAUI_DAWN_FETCH=ON`.

## Build

Build the framework only:

```sh
cmake -S . -B build
cmake --build build
```

Build demos and tests:

```sh
cmake -S . -B build -DLAMBDAUI_BUILD_DEMOS=ON -DLAMBDAUI_BUILD_TESTS=ON
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
cmake -S . -B build -DLAMBDAUI_PLATFORM=AUTO
cmake -S . -B build -DLAMBDAUI_PLATFORM=MACOS
cmake -S . -B build -DLAMBDAUI_PLATFORM=LINUX_WAYLAND
```

`AUTO` selects `MACOS` on Apple hosts and `LINUX_WAYLAND` on Linux/Unix hosts.

The renderer switch is `LAMBDAUI_RENDERER`:

```sh
cmake -S . -B build -DLAMBDAUI_RENDERER=AUTO
cmake -S . -B build -DLAMBDAUI_RENDERER=NATIVE
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=AUTO -DCMAKE_PREFIX_PATH=/path/to/dawn/install
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=WEBGPU -DCMAKE_PREFIX_PATH=/path/to/dawn/install
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=WEBGPU -DLAMBDAUI_DAWN_SOURCE_DIR=/path/to/dawn
cmake -S . -B build-webgpu -DLAMBDAUI_RENDERER=WEBGPU -DLAMBDAUI_DAWN_FETCH=ON
cmake -S . -B build-webgpu-only -DLAMBDAUI_RENDERER=AUTO -DLAMBDAUI_ENABLE_NATIVE_RENDERERS=OFF -DLAMBDAUI_DAWN_FETCH=ON
```

`AUTO` is the default. It selects `WEBGPU` when Dawn is explicitly configured or discoverable through `CMAKE_PREFIX_PATH`, and falls back to `NATIVE` while the Dawn/WebGPU renderer is being ported. Set `LAMBDAUI_ENABLE_NATIVE_RENDERERS=OFF` to reject that fallback and require Dawn/WebGPU. WebGPU builds define `LAMBDAUI_WEBGPU=1` and do not require Vulkan/libdrm/glslang on Linux.

Platform defines exported to consumers:

- `LAMBDAUI_NATIVE_RENDERERS=1`, `LAMBDAUI_METAL=1`, `LAMBDAUI_VULKAN=0`, `LAMBDAUI_WEBGPU=0` on native macOS builds.
- `LAMBDAUI_NATIVE_RENDERERS=1`, `LAMBDAUI_METAL=0`, `LAMBDAUI_VULKAN=1`, `LAMBDAUI_WEBGPU=0` on native Linux Wayland builds.
- `LAMBDAUI_NATIVE_RENDERERS=0`, `LAMBDAUI_METAL=0`, `LAMBDAUI_VULKAN=0`, `LAMBDAUI_WEBGPU=1` on WebGPU builds.

Use these defines to guard platform-specific APIs. Legacy Metal/Vulkan entry points such as Vulkan image import require both `LAMBDAUI_NATIVE_RENDERERS=1` and their backend define. WebGPU render targets use `WebGpuRenderTargetSpec`; set its `device` and `textureView` fields to render into a caller-owned Dawn/WebGPU texture view.

## Useful Build Options

- `LAMBDAUI_BUILD_DEMOS`: build standalone demos under `demos/`.
- `LAMBDAUI_RENDERER`: select `AUTO`, `NATIVE`, or `WEBGPU`.
- `LAMBDAUI_ENABLE_NATIVE_RENDERERS`: allow the legacy Metal/Vulkan renderers as a temporary fallback while WebGPU replaces them.
- `LAMBDAUI_DAWN_SOURCE_DIR`: optional Dawn source checkout for WebGPU builds.
- `LAMBDAUI_DAWN_FETCH`: fetch Dawn with CMake `FetchContent` when an installed package or source checkout is not provided.
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
ctest --test-dir build --output-on-failure
```

Platform-specific tests are included only when their backend is enabled. For example, Metal canvas tests build on native macOS, while Vulkan render-target tests build for native Linux Vulkan backends.

## Demos

Enable demos with `LAMBDAUI_BUILD_DEMOS=ON`. The demos exercise the main built-in controls and are a useful source of copyable application code:

- `hello-world`: smallest app skeleton.
- `layout-demo`: stack, alignment, and layout behavior.
- `theme-demo`: theme tokens, inputs, controls, and scrolling.
- `animation-demo`: reactive animation and render-cache behavior.
- `scene-graph-demo`: direct retained scene-graph use.
- Control demos such as `button-demo`, `textinput-demo`, `table-demo`, `popover-demo`, `toast-demo`, `slider-demo`, and `select-demo`.

## Troubleshooting

If macOS shader compilation fails, make sure full Xcode is selected:

```sh
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
sudo xcodebuild -license accept
```

If Xcode reports a missing Metal toolchain, install it:

```sh
xcodebuild -downloadComponent MetalToolchain
```

If Linux configuration fails, read the missing `pkg-config` package in the CMake error and install the matching development package.

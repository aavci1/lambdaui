#pragma once

#include <Lambda/Graphics/Canvas.hpp>
#include <Lambda/Core/Geometry.hpp>

#include <webgpu/webgpu.h>

#include <cstdint>
#include <memory>

struct wl_display;
struct wl_surface;

namespace lambdaui {

class TextSystem;

namespace webgpu {

struct WebGpuSurfaceSource {
  enum class Kind {
    None,
    MetalLayer,
    WaylandSurface,
  };

  static WebGpuSurfaceSource metalLayer(void* layer) noexcept {
    WebGpuSurfaceSource source{};
    source.kind = Kind::MetalLayer;
    source.metalLayerHandle = layer;
    return source;
  }

  static WebGpuSurfaceSource wayland(wl_display* display, wl_surface* surface) noexcept {
    WebGpuSurfaceSource source{};
    source.kind = Kind::WaylandSurface;
    source.waylandDisplay = display;
    source.waylandSurface = surface;
    return source;
  }

  Kind kind = Kind::None;
  void* metalLayerHandle = nullptr;
  wl_display* waylandDisplay = nullptr;
  wl_surface* waylandSurface = nullptr;
};

std::unique_ptr<Canvas> createWebGpuCanvas(WebGpuSurfaceSource surfaceSource,
                                           TextSystem& textSystem,
                                           Size initialSize,
                                           bool transparentSurface = false);

std::unique_ptr<Canvas> createWebGpuRenderTargetCanvas(TextSystem& textSystem,
                                                       Size logicalSize,
                                                       std::uint32_t pixelWidth,
                                                       std::uint32_t pixelHeight,
                                                       WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm);

std::unique_ptr<Canvas> createWebGpuRenderTargetCanvas(TextSystem& textSystem,
                                                       Size logicalSize,
                                                       std::uint32_t pixelWidth,
                                                       std::uint32_t pixelHeight,
                                                       WGPUDevice device,
                                                       WGPUQueue queue,
                                                       WGPUTextureFormat format);

std::unique_ptr<Canvas> createWebGpuExternalRenderTargetCanvas(TextSystem& textSystem,
                                                               Size logicalSize,
                                                               std::uint32_t pixelWidth,
                                                               std::uint32_t pixelHeight,
                                                               WGPUDevice device,
                                                               WGPUQueue queue,
                                                               WGPUTextureView textureView,
                                                               WGPUTextureFormat format);

} // namespace webgpu
} // namespace lambdaui

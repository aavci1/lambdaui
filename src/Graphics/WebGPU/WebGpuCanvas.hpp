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

struct WebGpuNativeSurface {
  enum class Kind {
    None,
    MetalLayer,
    WaylandSurface,
  };

  static WebGpuNativeSurface metalLayer(void* layer) noexcept {
    WebGpuNativeSurface native{};
    native.kind = Kind::MetalLayer;
    native.metalLayerHandle = layer;
    return native;
  }

  static WebGpuNativeSurface wayland(wl_display* display, wl_surface* surface) noexcept {
    WebGpuNativeSurface native{};
    native.kind = Kind::WaylandSurface;
    native.waylandDisplay = display;
    native.waylandSurface = surface;
    return native;
  }

  Kind kind = Kind::None;
  void* metalLayerHandle = nullptr;
  wl_display* waylandDisplay = nullptr;
  wl_surface* waylandSurface = nullptr;
};

std::unique_ptr<Canvas> createWebGpuCanvas(WebGpuNativeSurface nativeSurface,
                                           unsigned int handle,
                                           TextSystem& textSystem,
                                           Size initialSize,
                                           bool transparentSurface = false);

std::unique_ptr<Canvas> createWebGpuRenderTargetCanvas(TextSystem& textSystem,
                                                       Size logicalSize,
                                                       std::uint32_t pixelWidth,
                                                       std::uint32_t pixelHeight);

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

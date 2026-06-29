#pragma once

#include <Lambda/Graphics/Canvas.hpp>
#include <Lambda/Core/Geometry.hpp>

#include <cstdint>
#include <memory>

namespace lambdaui {

class TextSystem;

namespace webgpu {

struct WebGpuNativeSurface {
  enum class Kind {
    None,
    MetalLayer,
    WaylandSurface,
  };

  Kind kind = Kind::None;
  void* display = nullptr;
  void* surface = nullptr;
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

} // namespace webgpu
} // namespace lambdaui

#pragma once

/// \file Lambda/Graphics/RenderTarget.hpp
///
/// Public render destination abstraction for window-backed, offscreen, and externally-owned GPU targets.

#include <Lambda/Graphics/Canvas.hpp>

#include <cstdint>
#include <memory>

#include <webgpu/webgpu.h>

namespace lambdaui {

namespace scenegraph {
class SceneGraph;
}

struct WebGpuRenderTargetSpec {
  Size logicalSize{};
  std::uint32_t pixelWidth = 0;
  std::uint32_t pixelHeight = 0;
  float dpiScale = 1.f;

  /// Optional caller-owned WebGPU device and render target.
  /// When `device` is set without `textureView`, Lambda creates an internal texture on that device.
  /// When `textureView` is set, `device` must be the device that created it; Lambda retains the view
  /// while the RenderTarget exists. If `queue` is null, Lambda uses the device's default queue.
  WGPUDevice device = nullptr;
  WGPUQueue queue = nullptr;
  WGPUTextureView textureView = nullptr;
  /// Color target format for Lambda-owned textures, or the format of `textureView`.
  WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
};

class RenderTarget {
public:
  explicit RenderTarget(WebGpuRenderTargetSpec const& spec);

  ~RenderTarget();

  RenderTarget(RenderTarget const&) = delete;
  RenderTarget& operator=(RenderTarget const&) = delete;
  RenderTarget(RenderTarget&&) noexcept;
  RenderTarget& operator=(RenderTarget&&) noexcept;

  [[nodiscard]] Canvas& canvas();

  void beginFrame();
  void endFrame();

  void renderScene(scenegraph::SceneGraph const& scene);

private:
  std::unique_ptr<Canvas> canvas_;
};

} // namespace lambdaui

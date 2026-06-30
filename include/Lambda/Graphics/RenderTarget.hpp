#pragma once

/// \file Lambda/Graphics/RenderTarget.hpp
///
/// Public render destination abstraction for window-backed, offscreen, and externally-owned GPU targets.

#include <Lambda/Config.hpp>
#include <Lambda/Graphics/Canvas.hpp>

#include <cstdint>
#include <memory>

#if LAMBDAUI_WEBGPU
#include <webgpu/webgpu.h>
#endif

namespace lambdaui {

namespace scenegraph {
class SceneGraph;
}

#if LAMBDAUI_WEBGPU
struct WebGpuRenderTargetSpec {
  Size logicalSize{};
  std::uint32_t pixelWidth = 0;
  std::uint32_t pixelHeight = 0;
  float dpiScale = 1.f;

  /// Optional caller-owned WebGPU render target. When set, `device` must be the
  /// device that created `textureView`; Lambda retains the view while the
  /// RenderTarget exists and submits drawing work to the device queue.
  WGPUDevice device = nullptr;
  WGPUQueue queue = nullptr;
  WGPUTextureView textureView = nullptr;
  WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
};
#endif

namespace platform {
class RenderTarget;
}

class RenderTarget {
public:
#if LAMBDAUI_WEBGPU
  explicit RenderTarget(WebGpuRenderTargetSpec const& spec);
#endif

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
  std::unique_ptr<platform::RenderTarget> impl_;
};

} // namespace lambdaui

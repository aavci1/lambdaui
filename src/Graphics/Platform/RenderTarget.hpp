#pragma once

#include <Lambda/Graphics/RenderTarget.hpp>

#include <memory>

namespace lambdaui::platform {

class RenderTarget {
public:
  virtual ~RenderTarget() = default;

  virtual Canvas& canvas() = 0;
  virtual void beginFrame() = 0;
  virtual void endFrame() = 0;
};

#if LAMBDAUI_NATIVE_RENDERERS && LAMBDAUI_VULKAN
std::unique_ptr<RenderTarget> createRenderTarget(VulkanRenderTargetSpec const& spec);
#endif

#if LAMBDAUI_NATIVE_RENDERERS && LAMBDAUI_METAL
std::unique_ptr<RenderTarget> createRenderTarget(MetalRenderTargetSpec const& spec);
#endif

#if LAMBDAUI_WEBGPU
std::unique_ptr<RenderTarget> createRenderTarget(WebGpuRenderTargetSpec const& spec);
#endif

} // namespace lambdaui::platform

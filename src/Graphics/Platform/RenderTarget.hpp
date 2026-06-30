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

std::unique_ptr<RenderTarget> createRenderTarget(WebGpuRenderTargetSpec const& spec);

} // namespace lambdaui::platform

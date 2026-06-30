#include <Lambda/Graphics/RenderTarget.hpp>
#include <Lambda/Graphics/TextSystem.hpp>

#include "Graphics/Platform/RenderTarget.hpp"
#include "Graphics/WebGPU/WebGpuCanvas.hpp"

#if LAMBDAUI_PLATFORM_MACOS
#include "Graphics/CoreTextSystem.hpp"
#else
#include "Graphics/Linux/FreeTypeTextSystem.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace lambdaui::platform {

namespace {

TextSystem& renderTargetTextSystem() {
#if LAMBDAUI_PLATFORM_MACOS
  static CoreTextSystem textSystem;
#else
  static FreeTypeTextSystem textSystem;
#endif
  return textSystem;
}

std::uint32_t pixelExtent(float logicalExtent, float dpiScale) {
  if (logicalExtent <= 0.f) {
    return 0;
  }
  float const scale = dpiScale > 0.f ? dpiScale : 1.f;
  return std::max(1u, static_cast<std::uint32_t>(std::ceil(logicalExtent * scale)));
}

class WebGpuRenderTarget final : public RenderTarget {
public:
  explicit WebGpuRenderTarget(WebGpuRenderTargetSpec const& spec)
      : logicalSize_(spec.logicalSize),
        pixelWidth_(spec.pixelWidth != 0 ? spec.pixelWidth : pixelExtent(spec.logicalSize.width, spec.dpiScale)),
        pixelHeight_(spec.pixelHeight != 0 ? spec.pixelHeight : pixelExtent(spec.logicalSize.height, spec.dpiScale)) {
    if (logicalSize_.width <= 0.f || logicalSize_.height <= 0.f || pixelWidth_ == 0 || pixelHeight_ == 0) {
      throw std::runtime_error("Lambda WebGPU RenderTarget requires positive logical and pixel dimensions");
    }
    if (spec.textureView) {
      if (!spec.device) {
        throw std::runtime_error("Lambda WebGPU RenderTarget external texture views require a WGPUDevice");
      }
      canvas_ = webgpu::createWebGpuExternalRenderTargetCanvas(renderTargetTextSystem(),
                                                               logicalSize_,
                                                               pixelWidth_,
                                                               pixelHeight_,
                                                               spec.device,
                                                               spec.queue,
                                                               spec.textureView,
                                                               spec.format);
    } else {
      canvas_ = webgpu::createWebGpuRenderTargetCanvas(renderTargetTextSystem(),
                                                       logicalSize_,
                                                       pixelWidth_,
                                                       pixelHeight_);
    }
    if (!canvas_) {
      throw std::runtime_error("Failed to create WebGPU RenderTarget canvas");
    }
  }

  Canvas& canvas() override { return *canvas_; }
  void beginFrame() override { canvas_->beginFrame(); }
  void endFrame() override { canvas_->present(); }

private:
  Size logicalSize_{};
  std::uint32_t pixelWidth_ = 0;
  std::uint32_t pixelHeight_ = 0;
  std::unique_ptr<Canvas> canvas_;
};

} // namespace

std::unique_ptr<RenderTarget> createRenderTarget(WebGpuRenderTargetSpec const& spec) {
  return std::make_unique<WebGpuRenderTarget>(spec);
}

} // namespace lambdaui::platform

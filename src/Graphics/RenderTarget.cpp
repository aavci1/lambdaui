#include <Lambda/Graphics/RenderTarget.hpp>
#include <Lambda/Graphics/TextSystem.hpp>

#include "Graphics/WebGPU/WebGpuCanvas.hpp"

#if LAMBDAUI_PLATFORM_MACOS
#include "Graphics/CoreTextSystem.hpp"
#else
#include "Graphics/Linux/FreeTypeTextSystem.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace lambdaui {

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

Canvas& checkedCanvas(std::unique_ptr<Canvas> const& canvas) {
  if (!canvas) {
    throw std::runtime_error("Lambda WebGPU RenderTarget canvas is unavailable");
  }
  return *canvas;
}

} // namespace

RenderTarget::RenderTarget(WebGpuRenderTargetSpec const& spec) {
  std::uint32_t const pixelWidth =
      spec.pixelWidth != 0 ? spec.pixelWidth : pixelExtent(spec.logicalSize.width, spec.dpiScale);
  std::uint32_t const pixelHeight =
      spec.pixelHeight != 0 ? spec.pixelHeight : pixelExtent(spec.logicalSize.height, spec.dpiScale);
  if (spec.logicalSize.width <= 0.f || spec.logicalSize.height <= 0.f || pixelWidth == 0 || pixelHeight == 0) {
    throw std::runtime_error("Lambda WebGPU RenderTarget requires positive logical and pixel dimensions");
  }
  if (spec.textureView) {
    if (!spec.device) {
      throw std::runtime_error("Lambda WebGPU RenderTarget external texture views require a WGPUDevice");
    }
    canvas_ = webgpu::createWebGpuExternalRenderTargetCanvas(renderTargetTextSystem(),
                                                             spec.logicalSize,
                                                             pixelWidth,
                                                             pixelHeight,
                                                             spec.device,
                                                             spec.queue,
                                                             spec.textureView,
                                                             spec.format);
  } else if (spec.device) {
    canvas_ = webgpu::createWebGpuRenderTargetCanvas(renderTargetTextSystem(),
                                                     spec.logicalSize,
                                                     pixelWidth,
                                                     pixelHeight,
                                                     spec.device,
                                                     spec.queue,
                                                     spec.format);
  } else if (spec.queue) {
    throw std::runtime_error("Lambda WebGPU RenderTarget queues require a WGPUDevice");
  } else {
    canvas_ = webgpu::createWebGpuRenderTargetCanvas(renderTargetTextSystem(),
                                                     spec.logicalSize,
                                                     pixelWidth,
                                                     pixelHeight,
                                                     spec.format);
  }
  checkedCanvas(canvas_);
}

RenderTarget::~RenderTarget() = default;
RenderTarget::RenderTarget(RenderTarget&&) noexcept = default;
RenderTarget& RenderTarget::operator=(RenderTarget&&) noexcept = default;

Canvas& RenderTarget::canvas() {
  return checkedCanvas(canvas_);
}

void RenderTarget::beginFrame() {
  checkedCanvas(canvas_).beginFrame();
}

void RenderTarget::endFrame() {
  checkedCanvas(canvas_).present();
}

} // namespace lambdaui

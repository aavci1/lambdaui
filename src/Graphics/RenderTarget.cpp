#include <Lambda/Graphics/RenderTarget.hpp>

#include "Graphics/Platform/RenderTarget.hpp"

#include <stdexcept>
#include <utility>

namespace lambdaui {

namespace {

platform::RenderTarget& checkedTarget(std::unique_ptr<platform::RenderTarget> const& impl) {
  if (!impl) {
    throw std::runtime_error("Lambda RenderTarget backend is unavailable");
  }
  return *impl;
}

} // namespace

#if LAMBDAUI_WEBGPU
RenderTarget::RenderTarget(WebGpuRenderTargetSpec const& spec)
    : impl_(platform::createRenderTarget(spec)) {
  checkedTarget(impl_);
}
#endif

RenderTarget::~RenderTarget() = default;
RenderTarget::RenderTarget(RenderTarget&&) noexcept = default;
RenderTarget& RenderTarget::operator=(RenderTarget&&) noexcept = default;

Canvas& RenderTarget::canvas() {
  return checkedTarget(impl_).canvas();
}

void RenderTarget::beginFrame() {
  checkedTarget(impl_).beginFrame();
}

void RenderTarget::endFrame() {
  checkedTarget(impl_).endFrame();
}

} // namespace lambdaui

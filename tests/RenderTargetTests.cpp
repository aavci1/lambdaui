#include <Lambda/Graphics/RenderTarget.hpp>
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>

TEST_CASE("WebGPU render target rejects a queue without a device") {
  lambdaui::WebGpuRenderTargetSpec spec{};
  spec.logicalSize = {16.f, 16.f};
  spec.queue = reinterpret_cast<WGPUQueue>(static_cast<std::uintptr_t>(1u));

  CHECK_THROWS_WITH_AS((void)lambdaui::RenderTarget{spec},
                       "Lambda WebGPU RenderTarget queues require a WGPUDevice",
                       std::runtime_error);
}

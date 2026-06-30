#pragma once

#include <webgpu/webgpu.h>

#include <memory>
#include <string>

#if LAMBDAUI_DAWN_LEGACY_NATIVE
namespace dawn::native {
class Instance;
} // namespace dawn::native
#endif

namespace lambdaui::webgpu {

class WebGpuContext {
public:
  WebGpuContext();
  explicit WebGpuContext(WGPUSurface compatibleSurface);
  explicit WebGpuContext(WGPUDevice device, WGPUQueue queue = nullptr);
  ~WebGpuContext();

  WebGpuContext(WebGpuContext const&) = delete;
  WebGpuContext& operator=(WebGpuContext const&) = delete;
  WebGpuContext(WebGpuContext&& other) noexcept;
  WebGpuContext& operator=(WebGpuContext&& other) noexcept;

  [[nodiscard]] WGPUInstance instance() const noexcept { return instance_; }
  [[nodiscard]] WGPUAdapter adapter() const noexcept { return adapter_; }
  [[nodiscard]] WGPUDevice device() const noexcept { return device_; }
  [[nodiscard]] WGPUQueue queue() const noexcept { return queue_; }
  [[nodiscard]] bool hasDevice() const noexcept { return device_ != nullptr; }

  void initializeDevice(WGPUSurface compatibleSurface);
  void initializeExternalDevice(WGPUDevice device, WGPUQueue queue = nullptr);

private:
  void createInstance();

  WGPUInstance instance_ = nullptr;
  WGPUAdapter adapter_ = nullptr;
  WGPUDevice device_ = nullptr;
  WGPUQueue queue_ = nullptr;
#if LAMBDAUI_DAWN_LEGACY_NATIVE
  std::unique_ptr<dawn::native::Instance> nativeInstance_{};
#endif
};

#if LAMBDAUI_DAWN_LEGACY_NATIVE
using WebGpuLabel = char const*;
#else
using WebGpuLabel = WGPUStringView;
#endif

[[nodiscard]] WebGpuLabel stringView(char const* value) noexcept;
[[nodiscard]] std::string stringFromView(WebGpuLabel value);

} // namespace lambdaui::webgpu

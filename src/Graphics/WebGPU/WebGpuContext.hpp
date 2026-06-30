#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>
#include <string>

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

  void initializeDevice(WGPUSurface compatibleSurface);
  void initializeExternalDevice(WGPUDevice device, WGPUQueue queue = nullptr);
  [[nodiscard]] bool hasDevice() const noexcept { return device_ != nullptr; }

private:
  void createInstance();

  WGPUInstance instance_ = nullptr;
  WGPUAdapter adapter_ = nullptr;
  WGPUDevice device_ = nullptr;
  WGPUQueue queue_ = nullptr;
};

[[nodiscard]] WGPUStringView stringView(char const* value) noexcept;
[[nodiscard]] std::string stringFromView(WGPUStringView value);

} // namespace lambdaui::webgpu

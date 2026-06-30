#pragma once

#ifndef LAMBDAUI_WEBGPU
#define LAMBDAUI_WEBGPU 0
#endif

#if LAMBDAUI_WEBGPU
#include <webgpu/webgpu.h>
#else
#include <span>
#include <vulkan/vulkan_core.h>
#endif

namespace lambdaui::platform {

class GpuSurfaceProvider {
public:
  virtual ~GpuSurfaceProvider() = default;

#if LAMBDAUI_WEBGPU
  virtual WGPUSurface createSurface(WGPUInstance instance, void* nativeHandle) = 0;
#else
  [[nodiscard]] virtual std::span<char const* const> requiredInstanceExtensions() const = 0;
  virtual VkSurfaceKHR createSurface(VkInstance instance, void* nativeHandle) = 0;
#endif
};

} // namespace lambdaui::platform

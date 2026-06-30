#pragma once

/// \file Lambda/Graphics/WebGpuContext.hpp
///
/// Public accessors for Dawn/WebGPU handles owned by a LambdaUI WebGPU canvas.

#include <Lambda/Config.hpp>

#if LAMBDAUI_WEBGPU

#include <webgpu/webgpu.h>

namespace lambdaui {

class Canvas;

struct WebGpuCanvasHandles {
  /// Borrowed device handle. Lambda owns the reference; callers must not release it.
  WGPUDevice device = nullptr;

  /// Borrowed queue handle for the canvas device. Lambda owns the reference; callers must not release it.
  WGPUQueue queue = nullptr;

  /// Current render target format for pipeline/resource compatibility.
  WGPUTextureFormat renderTargetFormat = WGPUTextureFormat_Undefined;
};

[[nodiscard]] WebGpuCanvasHandles webGpuCanvasHandles(Canvas const& canvas) noexcept;
[[nodiscard]] WGPUDevice webGpuDevice(Canvas const& canvas) noexcept;
[[nodiscard]] WGPUQueue webGpuQueue(Canvas const& canvas) noexcept;
[[nodiscard]] WGPUTextureFormat webGpuRenderTargetFormat(Canvas const& canvas) noexcept;

} // namespace lambdaui

#endif // LAMBDAUI_WEBGPU

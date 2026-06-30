#pragma once

#include "Graphics/WebGPU/WebGpuContext.hpp"

#include <Lambda/Graphics/Canvas.hpp>

#include <memory>

namespace lambdaui {

class TextSystem;

namespace webgpu {

struct WebGpuCanvasOptions {
  bool transparentSurface = false;
};

std::unique_ptr<Canvas> createWebGpuCanvas(WebGpuContext context,
                                           WGPUSurface surface,
                                           unsigned int handle,
                                           TextSystem& textSystem,
                                           WebGpuCanvasOptions options = {});

void setWebGpuCanvasResizeBoundsHint(Canvas* canvas, int logicalWidth, int logicalHeight);
bool setWebGpuCanvasTransparentSurface(Canvas* canvas, bool enabled);
bool webGpuCanvasUsesMailboxPresentMode(Canvas* canvas);

} // namespace webgpu
} // namespace lambdaui

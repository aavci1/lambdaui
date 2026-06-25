#pragma once

#include <Lambda/Graphics/Canvas.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace lambdaui {

class Image;
struct MetalRenderTargetSpec;
class TextSystem;
class Window;
struct MetalFrameRecorder;
struct MetalRecorderSlice;

/// Creates the Metal-backed canvas for a window (macOS only).
std::unique_ptr<Canvas> createMetalCanvas(Window* window, void* caMetalLayer, unsigned int handle,
                                          TextSystem& textSystem,
                                          std::function<void()> requestRedraw = {});

/// Creates a Metal canvas that renders into a caller-owned texture instead of a CAMetalLayer.
std::unique_ptr<Canvas> createMetalRenderTargetCanvas(MetalRenderTargetSpec const& spec,
                                                      TextSystem& textSystem);

/// When `sync` is true, the next `present()` uses commit → waitUntilScheduled → drawable present (resize-safe).
/// No-op if `canvas` is not a Metal-backed canvas.
void setSyncPresentForCanvas(Canvas* canvas, bool sync);

/// Waits for the most recently submitted Metal frame for this canvas to complete on the GPU.
/// No-op if `canvas` is not a Metal-backed canvas or no frame has been submitted yet.
void waitForCanvasLastPresentComplete(Canvas* canvas);

} // namespace lambdaui

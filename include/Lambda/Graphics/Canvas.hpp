#pragma once

/// \file Lambda/Graphics/Canvas.hpp
///
/// Part of the Lambda public API.


#include <Lambda/Core/Geometry.hpp>
#include <Lambda/Core/Color.hpp>
#include <Lambda/Graphics/Path.hpp>
#include <Lambda/Graphics/Styles.hpp>
#include <Lambda/Graphics/ImageFillMode.hpp>
#include <Lambda/Graphics/TextLayout.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace lambdaui {

class Image;
class Window;
class Canvas;

namespace scenegraph {
class Renderer;

class PreparedRenderOps;
}

namespace scenegraph {

class PreparedRenderOps {
public:
  virtual ~PreparedRenderOps() = default;
  virtual bool replay(Renderer& renderer) const;
  virtual bool replay(Canvas& canvas) const {
    (void)canvas;
    return false;
  }
  virtual bool reusableAfterReplayFailure() const { return true; }
};

}

enum class Backend : std::uint8_t { Metal, Vulkan, WebGPU };

struct RecordedOps {
  virtual ~RecordedOps() = default;
  virtual Backend backend() const noexcept = 0;
};

struct RecordedOpsReplaySlice {
  Backend backend;
  void const* native = nullptr;
};

class Canvas {
public:
  virtual ~Canvas();

  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;
  Canvas(Canvas&&) = delete;
  Canvas& operator=(Canvas&&) = delete;

  virtual Backend backend() const noexcept = 0;
  virtual unsigned int windowHandle() const = 0;

  virtual void resize(int width, int height) = 0;
  virtual void updateDpiScale(float scaleX, float scaleY) = 0;
  virtual float dpiScale() const noexcept = 0;

  virtual void beginFrame() = 0;
  virtual void present() = 0;

  // -------------------------------------------------------------------------
  // State stack
  // -------------------------------------------------------------------------

  virtual void save() = 0;
  virtual void restore() = 0;

  // -------------------------------------------------------------------------
  // Transform
  // -------------------------------------------------------------------------

  virtual void setTransform(Mat3 const& m) = 0;
  virtual void transform(Mat3 const& m) = 0;
  virtual void translate(Point offset) = 0;
  virtual void translate(float x, float y) = 0;
  virtual void scale(float sx, float sy) = 0;
  virtual void scale(float s) = 0;
  virtual void rotate(float radians) = 0;
  virtual void rotate(float radians, Point pivot) = 0;
  virtual Mat3 currentTransform() const = 0;

  // -------------------------------------------------------------------------
  // Clip
  // -------------------------------------------------------------------------

  virtual void clipRect(Rect rect, CornerRadius const& cornerRadius = CornerRadius{}, bool antiAlias = false) = 0;

  virtual Rect clipBounds() const = 0;
  virtual bool quickReject(Rect rect) const = 0;

  // -------------------------------------------------------------------------
  // Opacity / blend
  // -------------------------------------------------------------------------

  virtual void setOpacity(float opacity) = 0;
  virtual float opacity() const = 0;
  virtual void setBlendMode(BlendMode mode) = 0;
  virtual BlendMode blendMode() const = 0;

  // -------------------------------------------------------------------------
  // Drawing
  // -------------------------------------------------------------------------

  virtual void drawRect(Rect const& rect, CornerRadius const& cornerRadius, FillStyle const& fill,
                        StrokeStyle const& stroke, ShadowStyle const& shadow = ShadowStyle::none()) = 0;
  virtual void drawLine(Point from, Point to, StrokeStyle const& stroke) = 0;
  virtual void drawPath(Path const& path, FillStyle const& fill, StrokeStyle const& stroke,
                        ShadowStyle const& shadow = ShadowStyle::none()) = 0;
  virtual void drawCircle(Point center, float radius, FillStyle const& fill, StrokeStyle const& stroke) = 0;

  /// Draw laid-out text. `origin` is the layout box top-left (`TextLayout::measuredSize`).
  virtual void drawTextLayout(TextLayout const& layout, Point origin) = 0;

  /// Draw `src` sub-rect of the image (pixel space) into `dst` (logical space). UVs are derived as src/size.
  virtual void drawImage(Image const& image, Rect const& src, Rect const& dst, CornerRadius const& corners = {},
                         float opacity = 1.f) = 0;

  /// Repeat the image across `dst` with a repeat sampler (1 logical pixel ≈ 1 texel).
  virtual void drawImageTiled(Image const& image, Rect const& dst, CornerRadius const& corners = {},
                               float opacity = 1.f) = 0;

  void drawImage(Image const& image, Rect const& dst, ImageFillMode fillMode = ImageFillMode::Cover,
                 CornerRadius const& corners = {}, float opacity = 1.f);

  /// Backend-native device handle as `void*` (Metal: `id<MTLDevice>`, WebGPU: `WGPUDevice`).
  /// Use with `loadImage(path, canvas.gpuDevice())` when the active backend supports GPU uploads.
  virtual void* gpuDevice() const = 0;

  virtual bool requestNextFrameCapture() = 0;
  virtual bool takeCapturedFrame(std::vector<std::uint8_t>& out, std::uint32_t& width,
                                 std::uint32_t& height) = 0;
  virtual std::unique_ptr<RecordedOps> beginRecordedOpsCapture() = 0;
  virtual void endRecordedOpsCapture() = 0;
  virtual std::unique_ptr<scenegraph::PreparedRenderOps> finalizeRecordedOps(
      std::unique_ptr<RecordedOps> recorded) = 0;
  virtual bool replayRecordedOps(RecordedOps const& recorded,
                                 RecordedOpsReplaySlice const* slice = nullptr) = 0;
  virtual bool replayRecordedLocalOps(RecordedOps const& recorded,
                                      RecordedOpsReplaySlice const* slice = nullptr) = 0;

  virtual void clear(Color color = Colors::transparent) = 0;

protected:
  Canvas() = default;
};

using RasterizeDrawCallback = std::function<void(Canvas&, Rect)>;

/// Records \p draw into an offscreen texture and returns it as an Image.
///
/// `logicalSize` is in logical canvas units. `dpiScale` is the target pixels-per-logical-unit scale.
/// The callback receives a temporary target-local canvas with bounds `{0, 0, logicalSize}`.
/// Returns null when the active backend cannot rasterize or the requested size is empty.
std::shared_ptr<Image> rasterizeToImage(Canvas& canvas, Size logicalSize,
                                        RasterizeDrawCallback draw, float dpiScale);

} // namespace lambdaui

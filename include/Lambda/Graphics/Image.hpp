#pragma once

/// \file Lambda/Graphics/Image.hpp
///
/// Part of the Lambda public API.


#include <Lambda/Core/Geometry.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <webgpu/webgpu.h>

namespace lambdaui {

/// Abstract image reference; pixel dimensions drive UV normalization in `Canvas::drawImage`.
class Image : public std::enable_shared_from_this<Image> {
public:
  enum class PixelFormat : std::uint8_t {
    Rgba8888,
    Bgra8888,
  };

  virtual ~Image() = default;

  Image(Image const&) = delete;
  Image& operator=(Image const&) = delete;

  virtual Size size() const = 0;
  virtual bool premultipliedAlpha() const noexcept { return false; }

  /// Replace this image with same-size tightly packed 8-bit RGBA pixels.
  /// Returns false when the WebGPU image is immutable or the size does not match.
  virtual bool updateRgbaPixels(std::span<std::uint8_t const> rgbaPixels,
                                WGPUDevice webGpuDevice = nullptr,
                                WGPUQueue webGpuQueue = nullptr);

  /// Replace this image with same-size tightly packed 8-bit pixels in `format`.
  /// Returns false when the WebGPU image is immutable, the format is incompatible,
  /// or the size does not match.
  virtual bool updatePixels(std::span<std::uint8_t const> pixels,
                            PixelFormat format,
                            WGPUDevice webGpuDevice = nullptr,
                            WGPUQueue webGpuQueue = nullptr);

  /// Replace a same-format sub-rectangle with 8-bit pixels.
  /// `pixels` must contain either tightly packed rows or enough bytes for
  /// `sourceBytesPerRow * (height - 1) + width * 4`. Returns false
  /// when the WebGPU image cannot update regions, the format is incompatible, or
  /// the rectangle is outside the image bounds.
  virtual bool updatePixelsRegion(std::span<std::uint8_t const> pixels,
                                  PixelFormat format,
                                  std::uint32_t x,
                                  std::uint32_t y,
                                  std::uint32_t width,
                                  std::uint32_t height,
                                  WGPUDevice webGpuDevice = nullptr,
                                  std::uint32_t sourceBytesPerRow = 0,
                                  WGPUQueue webGpuQueue = nullptr);

  /// Create an image from tightly packed 8-bit RGBA pixels.
  /// `rgbaPixels` must contain exactly width * height * 4 bytes.
  /// `webGpuDevice` and `webGpuQueue` are optional borrowed handles for eager upload.
  static std::shared_ptr<Image> fromRgbaPixels(std::uint32_t width, std::uint32_t height,
                                               std::span<std::uint8_t const> rgbaPixels,
                                               WGPUDevice webGpuDevice = nullptr,
                                               WGPUQueue webGpuQueue = nullptr);

  /// Create an image from tightly packed 8-bit pixels in `format`.
  /// `pixels` must contain exactly width * height * 4 bytes.
  static std::shared_ptr<Image> fromPixels(std::uint32_t width,
                                           std::uint32_t height,
                                           std::span<std::uint8_t const> pixels,
                                           PixelFormat format,
                                           WGPUDevice webGpuDevice = nullptr,
                                           WGPUQueue webGpuQueue = nullptr);

  /// Create an image reference backed by a WebGPU texture view.
  /// The texture view must be a sampleable 2D color view. Lambda retains the view.
  static std::shared_ptr<Image> fromExternalWebGpu(WGPUTextureView textureView,
                                                   std::uint32_t width,
                                                   std::uint32_t height,
                                                   bool premultipliedAlpha = false);

protected:
  Image() = default;
};

/// CPU-side RGBA decode result. Safe to produce off the render thread.
struct DecodedImageRgba {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;
};

/// Downscales decoded RGBA pixels so the longest edge is at most \p maxLongEdge.
[[nodiscard]] lambdaui::DecodedImageRgba downscaleDecodedImageRgba(lambdaui::DecodedImageRgba image,
                                                               std::uint32_t maxLongEdge);

/// Decodes an image file to tightly packed RGBA pixels without creating a WebGPU image.
[[nodiscard]] std::optional<DecodedImageRgba> decodeImageRgbaFromFile(std::string_view path);
[[nodiscard]] std::optional<DecodedImageRgba> decodeImageRgbaFromFile(std::string_view path,
                                                                       std::uint32_t maxLongEdge);

/// Creates a WebGPU image from decoded RGBA pixels. Must run on the render thread.
[[nodiscard]] std::shared_ptr<Image> imageFromDecodedRgba(DecodedImageRgba const& decoded,
                                                          WGPUDevice webGpuDevice = nullptr,
                                                          WGPUQueue webGpuQueue = nullptr);

/// Loads an image from disk into a WebGPU image.
/// `webGpuDevice` and `webGpuQueue` must match the target canvas when eager upload is required.
std::shared_ptr<Image> loadImage(std::string_view path, WGPUDevice webGpuDevice = nullptr);
std::shared_ptr<Image> loadImage(std::string_view path, WGPUDevice webGpuDevice, WGPUQueue webGpuQueue);
std::shared_ptr<Image> loadImage(std::string_view path, WGPUDevice webGpuDevice, std::uint32_t maxLongEdge);
std::shared_ptr<Image> loadImage(std::string_view path,
                                 WGPUDevice webGpuDevice,
                                 WGPUQueue webGpuQueue,
                                 std::uint32_t maxLongEdge);

} // namespace lambdaui

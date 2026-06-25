#pragma once

/// \file Lambda/SceneGraph/RasterizedLayerCache.hpp
///
/// Shared image/size/dpi validity state for scene-graph offscreen layers.

#include <Lambda/Graphics/Image.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace lambdaui::scenegraph {

class RasterizedLayerCache {
  public:
    void invalidate() const noexcept {
        image_.reset();
        logicalSize_ = {};
        dpiScale_ = 0.f;
    }

    bool hasValid(Size logicalSize, float dpiScale) const noexcept {
        constexpr float eps = 1e-3f;
        return static_cast<bool>(image_) &&
               std::abs(logicalSize_.width - logicalSize.width) <= eps &&
               std::abs(logicalSize_.height - logicalSize.height) <= eps &&
               std::abs(dpiScale_ - dpiScale) <= eps;
    }

    std::shared_ptr<Image> image() const noexcept {
        return image_;
    }

    void setImage(std::shared_ptr<Image> image, Size logicalSize, float dpiScale) const {
        image_ = std::move(image);
        logicalSize_ = logicalSize;
        dpiScale_ = dpiScale;
    }

    void noteRasterized() const noexcept {
        ++rasterizeCount_;
    }

    std::uint64_t rasterizeCount() const noexcept {
        return rasterizeCount_;
    }

  private:
    mutable std::shared_ptr<Image> image_{};
    mutable Size logicalSize_{};
    mutable float dpiScale_ = 0.f;
    mutable std::uint64_t rasterizeCount_ = 0;
};

} // namespace lambdaui::scenegraph

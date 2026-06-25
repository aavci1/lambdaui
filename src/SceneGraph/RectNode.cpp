#include <Lambda/SceneGraph/RectNode.hpp>
#include <Lambda/SceneGraph/Renderer.hpp>

#include <algorithm>
#include <utility>

namespace lambdaui::scenegraph {

namespace {

Rect expandForShadow(Rect rect, ShadowStyle const &shadow) noexcept {
    float const blur = shadow.isNone() ? 0.f : shadow.radius;
    float const left = std::max(0.f, blur - shadow.offset.x);
    float const right = std::max(0.f, blur + shadow.offset.x);
    float const top = std::max(0.f, blur - shadow.offset.y);
    float const bottom = std::max(0.f, blur + shadow.offset.y);
    rect.x -= left;
    rect.y -= top;
    rect.width += left + right;
    rect.height += top + bottom;
    return rect;
}

} // namespace

RectNode::RectNode(Rect bounds, FillStyle fill, StrokeStyle stroke, CornerRadius cornerRadius,
                   ShadowStyle shadow)
    : SceneNode(SceneNodeKind::Rect, bounds)
    , fill_(std::move(fill))
    , stroke_(std::move(stroke))
    , cornerRadius_(cornerRadius)
    , shadow_(shadow) {}

RectNode::~RectNode() = default;

FillStyle const &RectNode::fill() const noexcept {
    return fill_;
}

StrokeStyle const &RectNode::stroke() const noexcept {
    return stroke_;
}

CornerRadius RectNode::cornerRadius() const noexcept {
    return cornerRadius_;
}

ShadowStyle const &RectNode::shadow() const noexcept {
    return shadow_;
}

bool RectNode::clipsContents() const noexcept {
    return clipsContents_;
}

float RectNode::opacity() const noexcept {
    return opacity_;
}

bool RectNode::flattenOpacity() const noexcept {
    return flattenOpacity_;
}

void RectNode::setFill(FillStyle fill) {
    if (fill_ == fill) {
        return;
    }
    fill_ = std::move(fill);
    invalidateOpacityLayerCache();
    markDirty();
}

void RectNode::setStroke(StrokeStyle stroke) {
    if (stroke_ == stroke) {
        return;
    }
    stroke_ = std::move(stroke);
    invalidateOpacityLayerCache();
    markDirty();
}

void RectNode::setCornerRadius(CornerRadius cornerRadius) {
    if (cornerRadius_ == cornerRadius) {
        return;
    }
    cornerRadius_ = cornerRadius;
    invalidateOpacityLayerCache();
    markDirty();
}

void RectNode::setShadow(ShadowStyle shadow) {
    if (shadow_ == shadow) {
        return;
    }
    shadow_ = shadow;
    invalidateOpacityLayerCache();
    markDirty();
}

void RectNode::setClipsContents(bool clipsContents) noexcept {
    if (clipsContents_ == clipsContents) {
        return;
    }
    clipsContents_ = clipsContents;
    invalidateOpacityLayerCache();
    markSubtreeDirty();
}

void RectNode::setOpacity(float opacity) noexcept {
    float const next = std::clamp(opacity, 0.f, 1.f);
    if (opacity_ == next) {
        return;
    }
    opacity_ = next;
    markSubtreeDirty();
}

void RectNode::setFlattenOpacity(bool flatten) noexcept {
    if (flattenOpacity_ == flatten) {
        return;
    }
    flattenOpacity_ = flatten;
    invalidateOpacityLayerCache();
    markSubtreeDirty();
}

void RectNode::invalidateOpacityLayerCache() const {
    opacityLayerCache_.invalidate();
}

bool RectNode::hasValidOpacityLayerCache(Size logicalSize, float dpiScale) const noexcept {
    return opacityLayerCache_.hasValid(logicalSize, dpiScale);
}

std::shared_ptr<Image> RectNode::opacityLayerImage() const noexcept {
    return opacityLayerCache_.image();
}

void RectNode::setOpacityLayerImage(std::shared_ptr<Image> image, Size logicalSize, float dpiScale) const {
    opacityLayerCache_.setImage(std::move(image), logicalSize, dpiScale);
}

void RectNode::noteOpacityLayerRasterized() const {
    opacityLayerCache_.noteRasterized();
}

std::uint64_t RectNode::opacityLayerRasterizeCount() const noexcept {
    return opacityLayerCache_.rasterizeCount();
}

Rect RectNode::localBounds() const noexcept {
    return expandForShadow(Rect::sharp(0.f, 0.f, size().width, size().height), shadow_);
}

void RectNode::render(Renderer &renderer) const {
    Rect const bounds = Rect::sharp(0.f, 0.f, size().width, size().height);
    renderer.drawRect(bounds, cornerRadius_, fill_, stroke_, shadow_);
}

} // namespace lambdaui::scenegraph

#include <Lambda/UI/MountContext.hpp>

#include <Lambda/Graphics/TextSystem.hpp>
#include <Lambda/SceneGraph/SceneNode.hpp>
#include <Lambda/SceneGraph/RectNode.hpp>
#include <Lambda/SceneGraph/TextNode.hpp>
#include <Lambda/UI/Element.hpp>
#include <Lambda/UI/MeasureContext.hpp>
#include <Lambda/UI/Theme.hpp>
#include <Lambda/UI/Detail/MountPosition.hpp>
#include <Lambda/UI/Views/HStack.hpp>
#include <Lambda/UI/Views/ControlFlowDetail.hpp>
#include <Lambda/UI/Views/Rectangle.hpp>
#include <Lambda/UI/Views/Text.hpp>
#include <Lambda/UI/Views/VStack.hpp>
#include <Lambda/UI/Views/ZStack.hpp>

#include <Lambda/Reactive/Effect.hpp>

#include "Layout/Algorithms/StackLayout.hpp"
#include "UI/ViewLayout/ContainerScope.hpp"
#include "Layout/LayoutHelpers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lambdaui {

namespace {

thread_local MountContext* sCurrentMountContext = nullptr;

constexpr bool kTextContentChangesTriggerReflow = true;

TextLayoutOptions textLayoutOptions(Text const& text) {
  TextLayoutOptions options{};
  options.horizontalAlignment = text.horizontalAlignment;
  options.verticalAlignment = text.verticalAlignment;
  options.wrapping = text.wrapping;
  options.maxLines = text.maxLines;
  options.firstBaselineOffset = text.firstBaselineOffset;
  return options;
}

bool canUseDirectTextLayout(TextLayoutOptions const& options) noexcept {
  return options.horizontalAlignment == HorizontalAlignment::Leading &&
         options.verticalAlignment == VerticalAlignment::Top;
}

float directTextMaxWidth(TextLayoutOptions const& options, Rect const& box) noexcept {
  return options.wrapping == TextWrapping::NoWrap ? 0.f : box.width;
}

bool sameTextBox(Rect const& a, Rect const& b) noexcept {
  constexpr float epsilon = 0.01f;
  return std::fabs(a.x - b.x) <= epsilon &&
         std::fabs(a.y - b.y) <= epsilon &&
         std::fabs(a.width - b.width) <= epsilon &&
         std::fabs(a.height - b.height) <= epsilon;
}

bool sameLayoutScalar(float a, float b) noexcept {
  constexpr float epsilon = 0.01f;
  if (a == b) {
    return true;
  }
  if (!std::isfinite(a) || !std::isfinite(b)) {
    return false;
  }
  return std::fabs(a - b) <= epsilon;
}

bool sameTextLayoutGeometry(TextLayoutOptions const& options,
                            Rect const& a,
                            Rect const& b) noexcept {
  if (!canUseDirectTextLayout(options)) {
    return sameTextBox(a, b);
  }
  return sameLayoutScalar(directTextMaxWidth(options, a),
                          directTextMaxWidth(options, b));
}

float finiteOrZero(float value) {
  return std::isfinite(value) ? std::max(0.f, value) : 0.f;
}

Size assignedSize(LayoutConstraints const& constraints) {
  Size size{};
  if (std::isfinite(constraints.maxWidth)) {
    size.width = std::max(constraints.minWidth, constraints.maxWidth);
  } else {
    size.width = std::max(0.f, constraints.minWidth);
  }
  if (std::isfinite(constraints.maxHeight)) {
    size.height = std::max(constraints.minHeight, constraints.maxHeight);
  } else {
    size.height = std::max(0.f, constraints.minHeight);
  }
  return size;
}

bool positiveFinite(float value) {
  return std::isfinite(value) && value > 0.f;
}

float finiteSpan(float value) {
  return positiveFinite(value) ? value : 0.f;
}

bool axisStretches(std::optional<Alignment> const& alignment) {
  return alignment.has_value() && *alignment == Alignment::Stretch;
}

bool finiteWidthIsAssigned(LayoutHints const& hints) {
  return !hints.zStackHorizontalAlign.has_value() || axisStretches(hints.zStackHorizontalAlign);
}

bool fixedFiniteHeight(LayoutConstraints const& constraints) {
  return positiveFinite(constraints.maxHeight) && constraints.minHeight >= constraints.maxHeight - 1e-4f;
}

float textMeasureWidth(TextWrapping wrapping, LayoutConstraints const& constraints) {
  if (wrapping == TextWrapping::NoWrap) {
    return 0.f;
  }
  return std::isfinite(constraints.maxWidth) ? constraints.maxWidth : 0.f;
}

Size measuredTextFrameSize(TextSystem& textSystem,
                           std::string const& text,
                           Font const& font,
                           Color const& color,
                           TextLayoutOptions const& options,
                           TextWrapping wrapping,
                           LayoutConstraints const& constraints) {
  Size size = textSystem.measure(text, font, color, textMeasureWidth(wrapping, constraints), options);
  if (std::isfinite(constraints.maxWidth) && constraints.maxWidth > 0.f) {
    size.width = std::min(size.width, constraints.maxWidth);
  }
  if (std::isfinite(constraints.maxHeight) && constraints.maxHeight > 0.f) {
    size.height = std::min(size.height, constraints.maxHeight);
  }
  size.width = std::max(size.width, constraints.minWidth);
  size.height = std::max(size.height, constraints.minHeight);
  return size;
}

struct TextLayoutResult {
  Size size{};
  Rect box{};
  std::shared_ptr<TextLayout const> layout{};
  bool resized = false;
};

TextLayoutResult computeTextLayout(TextSystem& textSystem,
                                   std::string const& content,
                                   Font const& font,
                                   Color const& color,
                                   TextLayoutOptions const& options,
                                   TextWrapping wrapping,
                                   LayoutConstraints const& constraints,
                                   Size currentSize,
                                   bool allowResize = kTextContentChangesTriggerReflow) {
  Size nextSize = currentSize;
  if (allowResize || nextSize.width <= 0.f || nextSize.height <= 0.f) {
    nextSize = measuredTextFrameSize(textSystem, content, font, color, options,
                                     wrapping, constraints);
  }

  Rect const box{0.f, 0.f, finiteOrZero(nextSize.width), finiteOrZero(nextSize.height)};
  auto layout = canUseDirectTextLayout(options)
                    ? textSystem.layout(content, font, color, directTextMaxWidth(options, box), options)
                    : textSystem.layout(content, font, color, box, options);
  return TextLayoutResult{
      .size = Size{box.width, box.height},
      .box = box,
      .layout = std::move(layout),
      .resized = !sameLayoutScalar(currentSize.width, box.width) ||
                 !sameLayoutScalar(currentSize.height, box.height),
  };
}

bool sameTextLayoutInputs(std::string const& text,
                          Color const& color,
                          Font const& font,
                          Rect const& box,
                          std::string const& lastText,
                          Color const& lastColor,
                          Font const& lastFont,
                          Rect const& lastBox,
                          TextLayoutOptions const& options) noexcept {
  return text == lastText && color == lastColor && font == lastFont &&
         sameTextLayoutGeometry(options, box, lastBox);
}

void relayoutNearestLayoutOwner(scenegraph::SceneNode& node) {
  for (scenegraph::SceneNode* current = node.parent(); current; current = current->parent()) {
    if (!current->hasLayoutConstraints()) {
      continue;
    }
    current->invalidateSubtreeLayout();
    (void)current->relayoutStoredConstraints();
    return;
  }
}

bool finiteHeightIsConstrained(LayoutConstraints const& constraints, LayoutHints const& hints) {
  return axisStretches(hints.zStackVerticalAlign) ||
         (!hints.zStackVerticalAlign.has_value() && fixedFiniteHeight(constraints));
}

LayoutConstraints fixedConstraints(Size size) {
  return LayoutConstraints{
      .maxWidth = std::max(0.f, size.width),
      .maxHeight = std::max(0.f, size.height),
      .minWidth = std::max(0.f, size.width),
      .minHeight = std::max(0.f, size.height),
  };
}

void relayoutToFixedSizeIfNeeded(scenegraph::SceneNode& node, Size const& size) {
  LayoutConstraints const constraints = fixedConstraints(size);
  Size const currentSize = node.size();
  if (sameLayoutScalar(currentSize.width, size.width) &&
      sameLayoutScalar(currentSize.height, size.height)) {
    node.setLayoutConstraints(constraints);
    return;
  }
  (void)node.relayout(constraints, false);
  node.setLayoutConstraints(constraints);
}

LayoutConstraints stackChildConstraints(LayoutConstraints constraints) {
  constraints.minWidth = 0.f;
  constraints.minHeight = 0.f;
  return constraints;
}

Size measureChild(Element const& child, MountContext& ctx, LayoutConstraints const& constraints,
                  LayoutHints const& hints = {}) {
  ctx.measureContext().pushConstraints(constraints, hints);
  Size measured = child.measure(ctx.measureContext(), constraints, hints, ctx.textSystem());
  ctx.measureContext().popConstraints();
  return measured;
}

std::vector<layout::StackMainAxisChild> stackChildrenForAxis(std::vector<Element> const& children,
                                                             std::vector<Size> const& sizes,
                                                             std::vector<std::size_t> const& indices,
                                                             layout::StackAxis axis) {
  std::vector<layout::StackMainAxisChild> stackChildren;
  stackChildren.reserve(indices.size());
  for (std::size_t const i : indices) {
    float const naturalMainSize = axis == layout::StackAxis::Vertical ? sizes[i].height : sizes[i].width;
    stackChildren.push_back(layout::StackMainAxisChild{
        .naturalMainSize = naturalMainSize,
        .flexBasis = children[i].flexBasis(),
        .minMainSize = children[i].minMainSize(),
        .flexGrow = children[i].flexGrow(),
        .flexShrink = children[i].flexShrink(),
    });
  }
  return stackChildren;
}

bool collapsedStackChild(Element const& child, Size size) {
  return size.width <= layout::kFlexEpsilon &&
         size.height <= layout::kFlexEpsilon &&
         child.flexGrow() <= layout::kFlexEpsilon &&
         !child.flexBasis().has_value() &&
         child.minMainSize() <= layout::kFlexEpsilon;
}

bool shouldMountStackChild(Element const& child, Size size) {
  return !collapsedStackChild(child, size) || child.mountsWhenCollapsed();
}

struct MountedLayoutChild {
  scenegraph::SceneNode* node = nullptr;
  Point layoutOrigin{};
  float flexGrow = 0.f;
  float flexShrink = 0.f;
  std::optional<float> flexBasis;
  float minMainSize = 0.f;
  bool mountsWhenCollapsed = false;
  bool fillsStack = false;
};

void setMountedLayoutPosition(MountedLayoutChild& child, Point origin) {
  if (!child.node) {
    return;
  }
  Point const current = child.node->position();
  Vec2 const localOffset{current.x - child.layoutOrigin.x,
                         current.y - child.layoutOrigin.y};
  child.node->setPosition(Point{origin.x + localOffset.x, origin.y + localOffset.y});
  child.layoutOrigin = origin;
}

bool collapsedMountedChild(MountedLayoutChild const& child, Size size) {
  return size.width <= layout::kFlexEpsilon &&
         size.height <= layout::kFlexEpsilon &&
         child.flexGrow <= layout::kFlexEpsilon &&
         !child.flexBasis.has_value() &&
         child.minMainSize <= layout::kFlexEpsilon;
}

bool shouldRelayoutMountedChild(MountedLayoutChild const& child, Size size) {
  return !collapsedMountedChild(child, size) || child.mountsWhenCollapsed;
}

std::vector<std::size_t> activeStackIndices(std::vector<Element> const& children,
                                            std::vector<Size> const& sizes) {
  std::vector<std::size_t> indices;
  indices.reserve(children.size());
  for (std::size_t i = 0; i < children.size(); ++i) {
    if (!collapsedStackChild(children[i], sizes[i])) {
      indices.push_back(i);
    }
  }
  return indices;
}

std::vector<Size> sizesForIndices(std::vector<Size> const& sizes,
                                  std::vector<std::size_t> const& indices) {
  std::vector<Size> active;
  active.reserve(indices.size());
  for (std::size_t const index : indices) {
    active.push_back(sizes[index]);
  }
  return active;
}

std::vector<std::size_t> activeMountedIndices(std::vector<MountedLayoutChild> const& children,
                                              std::vector<Size> const& sizes) {
  std::vector<std::size_t> indices;
  indices.reserve(children.size());
  for (std::size_t i = 0; i < children.size(); ++i) {
    if (!collapsedMountedChild(children[i], sizes[i])) {
      indices.push_back(i);
    }
  }
  return indices;
}

std::vector<layout::StackMainAxisChild>
stackMountedChildrenForAxis(std::vector<MountedLayoutChild> const& children,
                            std::vector<Size> const& sizes,
                            std::vector<std::size_t> const& indices,
                            layout::StackAxis axis) {
  std::vector<layout::StackMainAxisChild> stackChildren;
  stackChildren.reserve(indices.size());
  for (std::size_t const i : indices) {
    float const naturalMainSize = axis == layout::StackAxis::Vertical ? sizes[i].height : sizes[i].width;
    stackChildren.push_back(layout::StackMainAxisChild{
        .naturalMainSize = naturalMainSize,
        .flexBasis = children[i].flexBasis,
        .minMainSize = children[i].minMainSize,
        .flexGrow = children[i].flexGrow,
        .flexShrink = children[i].flexShrink,
    });
  }
  return stackChildren;
}

struct StackGeometry {
  layout::StackLayoutResult stackLayout;
  std::vector<std::optional<layout::StackSlot>> slotsByChild;
};

layout::StackMainAxisLayout computeStackMainAxisLayout(
    std::vector<layout::StackMainAxisChild> const& children,
    float spacing,
    float assignedMainSize,
    bool mainSizeConstrained,
    JustifyContent justifyContent) {
  return layout::layoutStackMainAxis(children, spacing, assignedMainSize,
                                     mainSizeConstrained, justifyContent);
}

StackGeometry completeStackGeometry(std::size_t childCount,
                                    std::vector<std::size_t> const& activeIndices,
                                    std::vector<Size> const& activeSizes,
                                    layout::StackMainAxisLayout const& mainLayout,
                                    layout::StackAxis axis,
                                    Alignment alignment,
                                    float assignedCrossSize,
                                    bool crossSizeConstrained) {
  layout::StackLayoutResult stackLayout =
      layout::layoutStack(axis, alignment, activeSizes, mainLayout.mainSizes,
                          mainLayout.itemSpacing, mainLayout.containerMainSize,
                          mainLayout.startOffset, assignedCrossSize, crossSizeConstrained);
  std::vector<std::optional<layout::StackSlot>> slotsByChild(childCount);
  for (std::size_t layoutIndex = 0; layoutIndex < activeIndices.size(); ++layoutIndex) {
    slotsByChild[activeIndices[layoutIndex]] = stackLayout.slots[layoutIndex];
  }
  return StackGeometry{
      .stackLayout = std::move(stackLayout),
      .slotsByChild = std::move(slotsByChild),
  };
}

StackGeometry computeStackGeometry(std::size_t childCount,
                                   std::vector<std::size_t> const& activeIndices,
                                   std::vector<Size> const& activeSizes,
                                   std::vector<layout::StackMainAxisChild> const& stackChildren,
                                   layout::StackAxis axis,
                                   Alignment alignment,
                                   float spacing,
                                   float assignedMainSize,
                                   bool mainSizeConstrained,
                                   float assignedCrossSize,
                                   bool crossSizeConstrained,
                                   JustifyContent justifyContent) {
  layout::StackMainAxisLayout const mainLayout =
      computeStackMainAxisLayout(stackChildren, spacing, assignedMainSize,
                                 mainSizeConstrained, justifyContent);
  return completeStackGeometry(childCount, activeIndices, activeSizes, mainLayout,
                               axis, alignment, assignedCrossSize, crossSizeConstrained);
}

float stackMainSize(layout::StackAxis axis, Size size) {
  return axis == layout::StackAxis::Vertical ? size.height : size.width;
}

Point stackActiveOrigin(layout::StackAxis axis, Point slotOrigin, Point collapsedOrigin) {
  if (axis == layout::StackAxis::Vertical) {
    return Point{slotOrigin.x, std::max(slotOrigin.y, collapsedOrigin.y)};
  }
  return Point{std::max(slotOrigin.x, collapsedOrigin.x), slotOrigin.y};
}

Point advanceStackOrigin(layout::StackAxis axis, Point origin, Size size, float spacing) {
  if (axis == layout::StackAxis::Vertical) {
    return Point{origin.x, origin.y + size.height + spacing};
  }
  return Point{origin.x + size.width + spacing, origin.y};
}

void applyMountedStackGeometry(scenegraph::SceneNode& group,
                               std::vector<MountedLayoutChild>& mountedChildren,
                               std::vector<Size> const& sizes,
                               StackGeometry const& geometry,
                               scenegraph::LayoutFlow flow,
                               layout::StackAxis axis,
                               float spacing) {
  Point nextCollapsedOrigin{};
  for (std::size_t childIndex = 0; childIndex < mountedChildren.size(); ++childIndex) {
    MountedLayoutChild& child = mountedChildren[childIndex];
    std::optional<layout::StackSlot> const& slot = geometry.slotsByChild[childIndex];
    bool const active = slot.has_value();
    if (active && child.node) {
      Point const layoutOrigin = stackActiveOrigin(axis, slot->origin, nextCollapsedOrigin);
      relayoutToFixedSizeIfNeeded(*child.node, slot->assignedSize);
      setMountedLayoutPosition(child, layoutOrigin);
      nextCollapsedOrigin = advanceStackOrigin(axis, layoutOrigin, slot->assignedSize, spacing);
    } else if (shouldRelayoutMountedChild(child, sizes[childIndex])) {
      setMountedLayoutPosition(child, nextCollapsedOrigin);
      Size const mountedSize = child.node ? child.node->size() : Size{};
      if (stackMainSize(axis, mountedSize) > layout::kFlexEpsilon) {
        nextCollapsedOrigin = advanceStackOrigin(axis, nextCollapsedOrigin, mountedSize, spacing);
      }
    }
  }

  Size const mountedExtents = detail::controlStackExtents(group, flow);
  group.setSize(Size{
      finiteOrZero(std::max(geometry.stackLayout.containerSize.width, mountedExtents.width)),
      finiteOrZero(std::max(geometry.stackLayout.containerSize.height, mountedExtents.height)),
  });
}

void rewindMeasuredChildren(MountContext& ctx) {
  ctx.measureContext().rewindChildKeyIndex();
}

void layoutMountedVStack(scenegraph::SceneNode& group,
                         std::vector<MountedLayoutChild>& mountedChildren,
                         float spacing,
                         Alignment alignment,
                         JustifyContent justifyContent,
                         LayoutHints const& hints,
                         LayoutConstraints const& constraints) {
  float const nextAssignedWidth = finiteSpan(constraints.maxWidth);
  bool const widthAssigned = nextAssignedWidth > 0.f && finiteWidthIsAssigned(hints);
  float const nextAssignedHeight = finiteSpan(constraints.maxHeight);
  bool const heightConstrained = nextAssignedHeight > 0.f &&
                                 finiteHeightIsConstrained(constraints, hints);

  LayoutConstraints childConstraints = stackChildConstraints(constraints);
  childConstraints.maxWidth = nextAssignedWidth > 0.f ? nextAssignedWidth
                                                      : std::numeric_limits<float>::infinity();
  childConstraints.maxHeight = std::numeric_limits<float>::infinity();
  layout::clampLayoutMinToMax(childConstraints);

  std::vector<Size> sizes;
  sizes.reserve(mountedChildren.size());
  for (MountedLayoutChild const& child : mountedChildren) {
    if (child.node && child.node->relayout(childConstraints)) {
      sizes.push_back(child.node->size());
    } else {
      sizes.push_back(child.node ? child.node->size() : Size{});
    }
  }

  std::vector<std::size_t> const activeIndices = activeMountedIndices(mountedChildren, sizes);
  std::vector<Size> const activeSizes = sizesForIndices(sizes, activeIndices);
  std::vector<layout::StackMainAxisChild> stackChildren =
      stackMountedChildrenForAxis(mountedChildren, sizes, activeIndices, layout::StackAxis::Vertical);
  StackGeometry const geometry =
      computeStackGeometry(mountedChildren.size(), activeIndices, activeSizes, stackChildren,
                           layout::StackAxis::Vertical, alignment, spacing, nextAssignedHeight,
                           heightConstrained, nextAssignedWidth, widthAssigned, justifyContent);
  applyMountedStackGeometry(group, mountedChildren, sizes, geometry,
                            scenegraph::LayoutFlow::VerticalStack,
                            layout::StackAxis::Vertical, spacing);
}

void layoutMountedHStack(scenegraph::SceneNode& group,
                         std::vector<MountedLayoutChild>& mountedChildren,
                         float spacing,
                         Alignment alignment,
                         JustifyContent justifyContent,
                         LayoutHints const& hints,
                         LayoutConstraints const& constraints) {
  float const nextAssignedWidth = finiteSpan(constraints.maxWidth);
  bool const widthConstrained = nextAssignedWidth > 0.f && finiteWidthIsAssigned(hints);
  float const nextAssignedHeight = finiteSpan(constraints.maxHeight);
  bool const heightConstrained = nextAssignedHeight > 0.f &&
                                 finiteHeightIsConstrained(constraints, hints);
  bool const stretchCrossAxis = alignment == Alignment::Stretch && heightConstrained;

  LayoutConstraints initialConstraints = stackChildConstraints(constraints);
  initialConstraints.maxWidth = std::numeric_limits<float>::infinity();
  initialConstraints.maxHeight = stretchCrossAxis ? nextAssignedHeight
                                                  : std::numeric_limits<float>::infinity();
  layout::clampLayoutMinToMax(initialConstraints);

  std::vector<Size> initialSizes;
  initialSizes.reserve(mountedChildren.size());
  for (MountedLayoutChild const& child : mountedChildren) {
    if (child.node && child.node->relayout(initialConstraints)) {
      initialSizes.push_back(child.node->size());
    } else {
      initialSizes.push_back(child.node ? child.node->size() : Size{});
    }
  }
  std::vector<std::size_t> const activeIndices = activeMountedIndices(mountedChildren, initialSizes);
  std::vector<layout::StackMainAxisChild> stackChildren =
      stackMountedChildrenForAxis(mountedChildren, initialSizes, activeIndices,
                                  layout::StackAxis::Horizontal);
  layout::StackMainAxisLayout const mainLayout =
      computeStackMainAxisLayout(stackChildren, spacing, nextAssignedWidth,
                                 widthConstrained, justifyContent);

  std::vector<Size> rowSizes;
  rowSizes.reserve(activeIndices.size());
  float rowInnerHeight = 0.f;
  for (std::size_t layoutIndex = 0; layoutIndex < activeIndices.size(); ++layoutIndex) {
    std::size_t const childIndex = activeIndices[layoutIndex];
    LayoutConstraints childMeasure = stackChildConstraints(constraints);
    childMeasure.maxWidth = layoutIndex < mainLayout.mainSizes.size()
                                ? mainLayout.mainSizes[layoutIndex]
                                : std::numeric_limits<float>::infinity();
    childMeasure.maxHeight = stretchCrossAxis ? nextAssignedHeight
                                              : std::numeric_limits<float>::infinity();
    layout::clampLayoutMinToMax(childMeasure);
    MountedLayoutChild const& child = mountedChildren[childIndex];
    if (child.node && child.node->relayout(childMeasure)) {
      rowSizes.push_back(child.node->size());
    } else {
      rowSizes.push_back(child.node ? child.node->size() : Size{});
    }
    rowInnerHeight = std::max(rowInnerHeight, rowSizes.back().height);
  }

  float const rowCrossSize = heightConstrained ? nextAssignedHeight : rowInnerHeight;
  StackGeometry const geometry =
      completeStackGeometry(mountedChildren.size(), activeIndices, rowSizes, mainLayout,
                            layout::StackAxis::Horizontal, alignment,
                            rowCrossSize, heightConstrained);
  applyMountedStackGeometry(group, mountedChildren, initialSizes, geometry,
                            scenegraph::LayoutFlow::HorizontalStack,
                            layout::StackAxis::Horizontal, spacing);
}

void layoutMountedZStack(scenegraph::SceneNode& group,
                         std::vector<MountedLayoutChild>& mountedChildren,
                         Alignment horizontalAlignment,
                         Alignment verticalAlignment,
                         LayoutConstraints const& constraints) {
  float const assignedWidth = finiteSpan(constraints.maxWidth);
  float const assignedHeight = finiteSpan(constraints.maxHeight);
  float width = assignedWidth;
  float height = assignedHeight;

  LayoutConstraints childMeasure = stackChildConstraints(constraints);
  childMeasure.maxWidth = assignedWidth > 0.f ? assignedWidth : std::numeric_limits<float>::infinity();
  childMeasure.maxHeight = assignedHeight > 0.f ? assignedHeight : std::numeric_limits<float>::infinity();
  layout::clampLayoutMinToMax(childMeasure);

  std::vector<Size> sizes;
  sizes.reserve(mountedChildren.size());
  for (MountedLayoutChild const& child : mountedChildren) {
    if (child.node && child.node->relayout(childMeasure)) {
      sizes.push_back(child.node->size());
    } else {
      sizes.push_back(child.node ? child.node->size() : Size{});
    }
    width = std::max(width, sizes.back().width);
    height = std::max(height, sizes.back().height);
  }
  if (assignedWidth > 0.f) {
    width = std::min(width, assignedWidth);
  }
  if (assignedHeight > 0.f) {
    height = std::min(height, assignedHeight);
  }

  for (std::size_t i = 0; i < mountedChildren.size(); ++i) {
    MountedLayoutChild& child = mountedChildren[i];
    Size childFrame = i < sizes.size() ? sizes[i] : Size{};
    if (horizontalAlignment == Alignment::Stretch || child.fillsStack) {
      childFrame.width = width;
    }
    if (verticalAlignment == Alignment::Stretch || child.fillsStack) {
      childFrame.height = height;
    }
    if (child.node) {
      relayoutToFixedSizeIfNeeded(*child.node, childFrame);
      setMountedLayoutPosition(child, Point{
          layout::hAlignOffset(childFrame.width, width, horizontalAlignment),
          layout::vAlignOffset(childFrame.height, height, verticalAlignment),
      });
    }
  }
  group.setSize(Size{finiteOrZero(width), finiteOrZero(height)});
}

} // namespace

MountContext::MountContext(Reactive::Scope& owner,
                           TextSystem& textSystem, MeasureContext& measureContext,
                           LayoutConstraints constraints, LayoutHints hints,
                           Reactive::SmallFn<void()> requestRedraw,
                           EnvironmentBinding environmentBinding)
    : owner_(&owner)
    , environmentBinding_(std::move(environmentBinding))
    , textSystem_(textSystem)
    , measureContext_(measureContext)
    , constraints_(constraints)
    , hints_(hints)
    , requestRedraw_(std::move(requestRedraw)) {}

MountContext::MountContext(std::shared_ptr<Reactive::Scope> owner,
                           TextSystem& textSystem, MeasureContext& measureContext,
                           LayoutConstraints constraints, LayoutHints hints,
                           Reactive::SmallFn<void()> requestRedraw,
                           EnvironmentBinding environmentBinding)
    : ownedOwner_(std::move(owner))
    , owner_(ownedOwner_.get())
    , environmentBinding_(std::move(environmentBinding))
    , textSystem_(textSystem)
    , measureContext_(measureContext)
    , constraints_(constraints)
    , hints_(hints)
    , requestRedraw_(std::move(requestRedraw)) {}

MountContext MountContext::childWithSharedScope(LayoutConstraints constraints,
                                                LayoutHints hints) const {
  return MountContext{owner(), textSystem_, measureContext_, constraints,
                      hints, requestRedraw_, environmentBinding_};
}

MountContext MountContext::childWithOwnScope(LayoutConstraints constraints,
                                             LayoutHints hints) const {
  auto childScope = std::make_shared<Reactive::Scope>();
  owner().onCleanup([childScope] {
    childScope->dispose();
  });
  return MountContext{std::move(childScope), textSystem_, measureContext_, constraints,
                      hints, requestRedraw_, environmentBinding_};
}

MountContext MountContext::childWithEnvironment(EnvironmentBinding environment,
                                                LayoutConstraints constraints,
                                                LayoutHints hints) const {
  return MountContext{owner(), textSystem_, measureContext_, constraints,
                      hints, requestRedraw_, std::move(environment)};
}

void MountContext::requestRedraw() const {
  if (requestRedraw_) {
    requestRedraw_();
  }
}

namespace detail {

MountContext* currentMountContext() noexcept {
  return sCurrentMountContext;
}

CurrentMountContextScope::CurrentMountContextScope(MountContext& ctx) noexcept
    : previous_(sCurrentMountContext) {
  sCurrentMountContext = &ctx;
}

CurrentMountContextScope::~CurrentMountContextScope() {
  sCurrentMountContext = previous_;
}

std::unique_ptr<scenegraph::SceneNode> mountRectangle(Rectangle const&, MountContext& ctx) {
  Size const size = assignedSize(ctx.constraints());
  auto node = std::make_unique<scenegraph::RectNode>(
      Rect{0.f, 0.f, finiteOrZero(size.width), finiteOrZero(size.height)});
  auto* rawNode = node.get();
  rawNode->setLayoutConstraints(ctx.constraints());
  rawNode->setRelayout([rawNode](LayoutConstraints const& constraints) {
    Size const nextSize = assignedSize(constraints);
    rawNode->setSize(Size{finiteOrZero(nextSize.width), finiteOrZero(nextSize.height)});
  });
  return node;
}

std::unique_ptr<scenegraph::SceneNode> mountText(Text const& text, MountContext& ctx) {
  std::optional<Reactive::Signal<Theme>> themeSignal = ctx.environmentBinding().signal<ThemeKey>();
  Theme const fallbackTheme = themeSignal ? themeSignal->peek() : ctx.environmentBinding().value<ThemeKey>();
  Theme const& theme = themeSignal ? themeSignal->peek() : fallbackTheme;
  Font const baseFont = text.font;
  Font const font = resolveFont(baseFont, theme.bodyFont, theme);
  Color const color = resolveColor(text.color.evaluate(), theme.labelColor, theme);
  TextLayoutOptions const options = textLayoutOptions(text);
  std::string const initialText = text.text.evaluate();

  TextLayoutResult initialLayout =
      computeTextLayout(ctx.textSystem(), initialText, font, color, options,
                        text.wrapping, ctx.constraints(), Size{}, true);
  auto node = std::make_unique<scenegraph::TextNode>(initialLayout.box, initialLayout.layout);

  auto* rawNode = node.get();
  rawNode->setLayoutConstraints(ctx.constraints());
  TextSystem* textSystemForRelayout = &ctx.textSystem();
  Reactive::Bindable<std::string> relayoutTextBinding = text.text;
  Reactive::Bindable<Color> relayoutColorBinding = text.color;
  rawNode->setRelayout([rawNode, relayoutTextBinding = std::move(relayoutTextBinding),
                        relayoutColorBinding = std::move(relayoutColorBinding),
                        textSystemForRelayout, baseFont, fallbackTheme, themeSignal, options,
                        wrapping = text.wrapping, lastText = initialText, lastColor = color,
                        lastFont = font, lastBox = initialLayout.box](LayoutConstraints const& constraints) mutable {
    Theme const& currentTheme = themeSignal ? themeSignal->peek() : fallbackTheme;
    Font const currentFont = resolveFont(baseFont, currentTheme.bodyFont, currentTheme);
    std::string const currentText = relayoutTextBinding.evaluate();
    Color const currentColor =
        resolveColor(relayoutColorBinding.evaluate(), currentTheme.labelColor, currentTheme);
    TextLayoutResult currentLayout =
        computeTextLayout(*textSystemForRelayout, currentText, currentFont, currentColor,
                          options, wrapping, constraints, rawNode->size(), true);
    rawNode->setSize(currentLayout.size);
    if (sameTextLayoutInputs(currentText, currentColor, currentFont, currentLayout.box,
                             lastText, lastColor, lastFont, lastBox, options)) {
      return;
    }
    rawNode->setLayout(std::move(currentLayout.layout));
    lastText = currentText;
    lastColor = currentColor;
    lastFont = currentFont;
    lastBox = currentLayout.box;
  });

  if (text.text.isReactive() || text.color.isReactive() || themeSignal.has_value()) {
    Reactive::Bindable<std::string> textBinding = text.text;
    Reactive::Bindable<Color> colorBinding = text.color;
    TextSystem* textSystem = &ctx.textSystem();
    LayoutConstraints constraints = ctx.constraints();
    TextWrapping wrapping = text.wrapping;
    Reactive::SmallFn<void()> requestRedraw = ctx.redrawCallback();
    Reactive::withOwner(ctx.owner(), [rawNode, textBinding = std::move(textBinding), textSystem,
                                      colorBinding = std::move(colorBinding), baseFont, fallbackTheme, themeSignal,
                                      options, constraints, wrapping,
                                      requestRedraw = std::move(requestRedraw)]() mutable {
      Reactive::Effect([rawNode, textBinding, colorBinding, textSystem, baseFont, fallbackTheme, themeSignal,
                        options, constraints, wrapping, requestRedraw,
                        lastText = std::optional<std::string>{}, lastColor = std::optional<Color>{},
                        lastFont = std::optional<Font>{}, lastBox = std::optional<Rect>{}]() mutable {
        Theme const& currentTheme = themeSignal ? themeSignal->get() : fallbackTheme;
        Font const currentFont = resolveFont(baseFont, currentTheme.bodyFont, currentTheme);
        std::string const currentText = textBinding.evaluate();
        Color const currentColor = resolveColor(colorBinding.evaluate(), currentTheme.labelColor, currentTheme);
        LayoutConstraints const currentConstraints =
            kTextContentChangesTriggerReflow
                ? constraints
                : (rawNode->hasLayoutConstraints() ? rawNode->layoutConstraints() : constraints);
        TextLayoutResult currentLayout =
            computeTextLayout(*textSystem, currentText, currentFont, currentColor,
                              options, wrapping, currentConstraints, rawNode->size(),
                              kTextContentChangesTriggerReflow);
        if (lastText && lastColor && lastFont && lastBox &&
            sameTextLayoutInputs(currentText, currentColor, currentFont, currentLayout.box,
                                 *lastText, *lastColor, *lastFont, *lastBox, options)) {
          return;
        }
        lastText = currentText;
        lastColor = currentColor;
        lastFont = currentFont;
        lastBox = currentLayout.box;
        rawNode->setSize(currentLayout.size);
        rawNode->setLayout(std::move(currentLayout.layout));
        if (currentLayout.resized && kTextContentChangesTriggerReflow) {
          relayoutNearestLayoutOwner(*rawNode);
        }
        if (requestRedraw) {
          requestRedraw();
        }
      });
    });
  }

  return node;
}

std::unique_ptr<scenegraph::SceneNode> mountVStack(VStack const& stack, MountContext& ctx) {
  ContainerMeasureScope scope(ctx.measureContext());
  float const assignedWidth = finiteSpan(ctx.constraints().maxWidth);
  bool const widthAssigned = assignedWidth > 0.f && finiteWidthIsAssigned(ctx.hints());
  float const assignedHeight = finiteSpan(ctx.constraints().maxHeight);
  bool const heightConstrained = assignedHeight > 0.f &&
                                 finiteHeightIsConstrained(ctx.constraints(), ctx.hints());

  LayoutConstraints childConstraints = stackChildConstraints(ctx.constraints());
  childConstraints.maxWidth = assignedWidth > 0.f ? assignedWidth
                                                  : std::numeric_limits<float>::infinity();
  childConstraints.maxHeight = std::numeric_limits<float>::infinity();
  layout::clampLayoutMinToMax(childConstraints);

  LayoutHints childHints{};
  childHints.vStackCrossAlign = stack.alignment;
  std::vector<Size> sizes;
  sizes.reserve(stack.children.size());
  for (Element const& child : stack.children) {
    sizes.push_back(measureChild(child, ctx, childConstraints, childHints));
  }
  std::vector<std::size_t> const activeIndices = activeStackIndices(stack.children, sizes);
  std::vector<Size> const activeSizes = sizesForIndices(sizes, activeIndices);

  std::vector<layout::StackMainAxisChild> stackChildren =
      stackChildrenForAxis(stack.children, sizes, activeIndices, layout::StackAxis::Vertical);
  StackGeometry const geometry =
      computeStackGeometry(stack.children.size(), activeIndices, activeSizes, stackChildren,
                           layout::StackAxis::Vertical, stack.alignment, stack.spacing,
                           assignedHeight, heightConstrained, assignedWidth, widthAssigned,
                           stack.justifyContent);

  auto group = std::make_unique<scenegraph::SceneNode>(
      Rect{0.f, 0.f, finiteOrZero(geometry.stackLayout.containerSize.width),
           finiteOrZero(geometry.stackLayout.containerSize.height)});
  group->setLayoutFlow(scenegraph::LayoutFlow::VerticalStack);
  group->setLayoutSpacing(stack.spacing);
  auto mountedChildren = std::make_shared<std::vector<MountedLayoutChild>>();

  Point nextCollapsedOrigin{};
  for (std::size_t childIndex = 0; childIndex < stack.children.size(); ++childIndex) {
    Element const& child = stack.children[childIndex];
    std::optional<layout::StackSlot> const& slot = geometry.slotsByChild[childIndex];
    bool const active = slot.has_value();
    if (!active && !shouldMountStackChild(child, sizes[childIndex])) {
      continue;
    }

    ctx.measureContext().setChildIndex(childIndex);
    MountContext childCtx = ctx.childWithSharedScope(childConstraints, childHints);
    auto node = child.mount(childCtx);
    Size mountedSize = active ? slot->assignedSize : Size{};
    Point const layoutOrigin =
        active ? stackActiveOrigin(layout::StackAxis::Vertical, slot->origin,
                                   nextCollapsedOrigin)
               : nextCollapsedOrigin;
    if (node) {
      if (active) {
        relayoutToFixedSizeIfNeeded(*node, slot->assignedSize);
      }
      detail::setLayoutPosition(*node, layoutOrigin);
      mountedSize = node->size();
      mountedChildren->push_back(MountedLayoutChild{
          .node = node.get(),
          .layoutOrigin = layoutOrigin,
          .flexGrow = child.flexGrow(),
          .flexShrink = child.flexShrink(),
          .flexBasis = child.flexBasis(),
          .minMainSize = child.minMainSize(),
          .mountsWhenCollapsed = child.mountsWhenCollapsed(),
          .fillsStack = child.is<Text>() || child.flexGrow() > 0.f,
      });
      group->appendChild(std::move(node));
    }
    if (active) {
      nextCollapsedOrigin = advanceStackOrigin(layout::StackAxis::Vertical, layoutOrigin,
                                               slot->assignedSize, stack.spacing);
    } else if (stackMainSize(layout::StackAxis::Vertical, mountedSize) > layout::kFlexEpsilon) {
      nextCollapsedOrigin = advanceStackOrigin(layout::StackAxis::Vertical, nextCollapsedOrigin,
                                               mountedSize, stack.spacing);
    }
  }
  Size const mountedExtents = controlStackExtents(*group, scenegraph::LayoutFlow::VerticalStack);
  Size const initialGroupSize = group->size();
  group->setSize(Size{finiteOrZero(std::max(initialGroupSize.width, mountedExtents.width)),
                      finiteOrZero(std::max(initialGroupSize.height, mountedExtents.height))});
  auto* rawGroup = group.get();
  float const spacing = stack.spacing;
  Alignment const alignment = stack.alignment;
  JustifyContent const justifyContent = stack.justifyContent;
  LayoutHints const relayoutHints = ctx.hints();
  rawGroup->setLayoutConstraints(ctx.constraints());
  rawGroup->setRelayout([rawGroup, mountedChildren, spacing, alignment,
                         justifyContent, relayoutHints](LayoutConstraints const& constraints) {
    layoutMountedVStack(*rawGroup, *mountedChildren, spacing, alignment,
                        justifyContent, relayoutHints, constraints);
  });
  return group;
}

std::unique_ptr<scenegraph::SceneNode> mountHStack(HStack const& stack, MountContext& ctx) {
  ContainerMeasureScope scope(ctx.measureContext());
  if (stack.children.empty()) {
    return std::make_unique<scenegraph::SceneNode>();
  }

  float const assignedWidth = finiteSpan(ctx.constraints().maxWidth);
  bool const widthConstrained = assignedWidth > 0.f && finiteWidthIsAssigned(ctx.hints());
  float const assignedHeight = finiteSpan(ctx.constraints().maxHeight);
  bool const heightConstrained = assignedHeight > 0.f &&
                                 finiteHeightIsConstrained(ctx.constraints(), ctx.hints());
  bool const stretchCrossAxis = stack.alignment == Alignment::Stretch && heightConstrained;

  LayoutConstraints initialConstraints = stackChildConstraints(ctx.constraints());
  initialConstraints.maxWidth = std::numeric_limits<float>::infinity();
  initialConstraints.maxHeight = stretchCrossAxis ? assignedHeight
                                                  : std::numeric_limits<float>::infinity();
  layout::clampLayoutMinToMax(initialConstraints);

  std::vector<Size> initialSizes;
  initialSizes.reserve(stack.children.size());
  for (Element const& child : stack.children) {
    initialSizes.push_back(measureChild(child, ctx, initialConstraints, LayoutHints{}));
  }
  std::vector<std::size_t> const activeIndices = activeStackIndices(stack.children, initialSizes);

  std::vector<layout::StackMainAxisChild> stackChildren =
      stackChildrenForAxis(stack.children, initialSizes, activeIndices, layout::StackAxis::Horizontal);
  layout::StackMainAxisLayout const mainLayout =
      computeStackMainAxisLayout(stackChildren, stack.spacing, assignedWidth,
                                 widthConstrained, stack.justifyContent);

  rewindMeasuredChildren(ctx);

  LayoutHints rowHints{};
  rowHints.hStackCrossAlign = stack.alignment;
  std::vector<Size> rowSizes;
  rowSizes.reserve(activeIndices.size());
  float rowInnerHeight = 0.f;
  for (std::size_t layoutIndex = 0; layoutIndex < activeIndices.size(); ++layoutIndex) {
    std::size_t const childIndex = activeIndices[layoutIndex];
    LayoutConstraints childMeasure = stackChildConstraints(ctx.constraints());
    childMeasure.maxWidth = layoutIndex < mainLayout.mainSizes.size()
                                ? mainLayout.mainSizes[layoutIndex]
                                : std::numeric_limits<float>::infinity();
    childMeasure.maxHeight = stretchCrossAxis ? assignedHeight
                                              : std::numeric_limits<float>::infinity();
    layout::clampLayoutMinToMax(childMeasure);
    ctx.measureContext().setChildIndex(childIndex);
    Size const size = measureChild(stack.children[childIndex], ctx, childMeasure, rowHints);
    rowSizes.push_back(size);
    rowInnerHeight = std::max(rowInnerHeight, size.height);
  }

  float const rowCrossSize = heightConstrained ? assignedHeight : rowInnerHeight;
  StackGeometry const geometry =
      completeStackGeometry(stack.children.size(), activeIndices, rowSizes, mainLayout,
                            layout::StackAxis::Horizontal, stack.alignment,
                            rowCrossSize, heightConstrained);

  auto group = std::make_unique<scenegraph::SceneNode>(
      Rect{0.f, 0.f, finiteOrZero(geometry.stackLayout.containerSize.width),
           finiteOrZero(geometry.stackLayout.containerSize.height)});
  group->setLayoutFlow(scenegraph::LayoutFlow::HorizontalStack);
  group->setLayoutSpacing(stack.spacing);
  auto mountedChildren = std::make_shared<std::vector<MountedLayoutChild>>();

  Point nextCollapsedOrigin{};
  for (std::size_t childIndex = 0; childIndex < stack.children.size(); ++childIndex) {
    Element const& child = stack.children[childIndex];
    std::optional<layout::StackSlot> const& slot = geometry.slotsByChild[childIndex];
    bool const active = slot.has_value();
    if (!active && !shouldMountStackChild(child, initialSizes[childIndex])) {
      continue;
    }

    ctx.measureContext().setChildIndex(childIndex);
    MountContext childCtx = ctx.childWithSharedScope(initialConstraints, rowHints);
    auto node = child.mount(childCtx);
    Size mountedSize = active ? slot->assignedSize : Size{};
    Point const layoutOrigin =
        active ? stackActiveOrigin(layout::StackAxis::Horizontal, slot->origin,
                                   nextCollapsedOrigin)
               : nextCollapsedOrigin;
    if (node) {
      if (active) {
        relayoutToFixedSizeIfNeeded(*node, slot->assignedSize);
      }
      detail::setLayoutPosition(*node, layoutOrigin);
      mountedSize = node->size();
      mountedChildren->push_back(MountedLayoutChild{
          .node = node.get(),
          .layoutOrigin = layoutOrigin,
          .flexGrow = child.flexGrow(),
          .flexShrink = child.flexShrink(),
          .flexBasis = child.flexBasis(),
          .minMainSize = child.minMainSize(),
          .mountsWhenCollapsed = child.mountsWhenCollapsed(),
          .fillsStack = child.is<Text>() || child.flexGrow() > 0.f,
      });
      group->appendChild(std::move(node));
    }
    if (active) {
      nextCollapsedOrigin = advanceStackOrigin(layout::StackAxis::Horizontal, layoutOrigin,
                                               slot->assignedSize, stack.spacing);
    } else if (stackMainSize(layout::StackAxis::Horizontal, mountedSize) > layout::kFlexEpsilon) {
      nextCollapsedOrigin = advanceStackOrigin(layout::StackAxis::Horizontal, nextCollapsedOrigin,
                                               mountedSize, stack.spacing);
    }
  }
  Size const mountedExtents = controlStackExtents(*group, scenegraph::LayoutFlow::HorizontalStack);
  Size const initialGroupSize = group->size();
  group->setSize(Size{finiteOrZero(std::max(initialGroupSize.width, mountedExtents.width)),
                      finiteOrZero(std::max(initialGroupSize.height, mountedExtents.height))});
  auto* rawGroup = group.get();
  float const spacing = stack.spacing;
  Alignment const alignment = stack.alignment;
  JustifyContent const justifyContent = stack.justifyContent;
  LayoutHints const relayoutHints = ctx.hints();
  rawGroup->setLayoutConstraints(ctx.constraints());
  rawGroup->setRelayout([rawGroup, mountedChildren, spacing, alignment,
                         justifyContent, relayoutHints](LayoutConstraints const& constraints) {
    layoutMountedHStack(*rawGroup, *mountedChildren, spacing, alignment,
                        justifyContent, relayoutHints, constraints);
  });
  return group;
}

std::unique_ptr<scenegraph::SceneNode> mountZStack(ZStack const& stack, MountContext& ctx) {
  ContainerMeasureScope scope(ctx.measureContext());
  float const assignedWidth = finiteSpan(ctx.constraints().maxWidth);
  float const assignedHeight = finiteSpan(ctx.constraints().maxHeight);
  float width = assignedWidth;
  float height = assignedHeight;

  LayoutConstraints childMeasure = stackChildConstraints(ctx.constraints());
  childMeasure.maxWidth = assignedWidth > 0.f ? assignedWidth : std::numeric_limits<float>::infinity();
  childMeasure.maxHeight = assignedHeight > 0.f ? assignedHeight : std::numeric_limits<float>::infinity();
  layout::clampLayoutMinToMax(childMeasure);

  LayoutHints childHints{};
  childHints.zStackHorizontalAlign = stack.horizontalAlignment;
  childHints.zStackVerticalAlign = stack.verticalAlignment;
  std::vector<Size> sizes;
  sizes.reserve(stack.children.size());
  for (Element const& child : stack.children) {
    Size const size = measureChild(child, ctx, childMeasure, childHints);
    sizes.push_back(size);
    width = std::max(width, size.width);
    height = std::max(height, size.height);
  }
  if (assignedWidth > 0.f) {
    width = std::min(width, assignedWidth);
  }
  if (assignedHeight > 0.f) {
    height = std::min(height, assignedHeight);
  }

  auto group = std::make_unique<scenegraph::SceneNode>(
      Rect{0.f, 0.f, finiteOrZero(width), finiteOrZero(height)});
  auto mountedChildren = std::make_shared<std::vector<MountedLayoutChild>>();

  for (std::size_t i = 0; i < stack.children.size(); ++i) {
    Element const& child = stack.children[i];
    Size childFrame = i < sizes.size() ? sizes[i] : Size{};
    bool const fillsStack = child.is<Text>() || child.flexGrow() > 0.f;
    if (stack.horizontalAlignment == Alignment::Stretch || fillsStack) {
      childFrame.width = width;
    }
    if (stack.verticalAlignment == Alignment::Stretch || fillsStack) {
      childFrame.height = height;
    }
    ctx.measureContext().setChildIndex(i);
    MountContext childCtx = ctx.childWithSharedScope(childMeasure, childHints);
    auto node = child.mount(childCtx);
    if (node) {
      relayoutToFixedSizeIfNeeded(*node, childFrame);
      Point const origin{
          layout::hAlignOffset(childFrame.width, width, stack.horizontalAlignment),
          layout::vAlignOffset(childFrame.height, height, stack.verticalAlignment),
      };
      detail::setLayoutPosition(*node, origin);
      mountedChildren->push_back(MountedLayoutChild{
          .node = node.get(),
          .layoutOrigin = origin,
          .flexGrow = child.flexGrow(),
          .flexShrink = child.flexShrink(),
          .flexBasis = child.flexBasis(),
          .minMainSize = child.minMainSize(),
          .mountsWhenCollapsed = child.mountsWhenCollapsed(),
          .fillsStack = fillsStack,
      });
      group->appendChild(std::move(node));
    }
  }
  auto* rawGroup = group.get();
  Alignment const horizontalAlignment = stack.horizontalAlignment;
  Alignment const verticalAlignment = stack.verticalAlignment;
  rawGroup->setLayoutConstraints(ctx.constraints());
  rawGroup->setRelayout([rawGroup, mountedChildren, horizontalAlignment,
                         verticalAlignment](LayoutConstraints const& constraints) {
    layoutMountedZStack(*rawGroup, *mountedChildren, horizontalAlignment,
                        verticalAlignment, constraints);
  });
  return group;
}

} // namespace detail
} // namespace lambdaui

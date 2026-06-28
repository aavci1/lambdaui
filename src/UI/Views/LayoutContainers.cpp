#include <Lambda/UI/Views/HStack.hpp>
#include <Lambda/UI/Views/VStack.hpp>
#include <Lambda/UI/Views/ZStack.hpp>

#include <Lambda/Graphics/TextSystem.hpp>
#include <Lambda/UI/MeasureContext.hpp>

#include "Layout/Algorithms/StackLayout.hpp"
#include "UI/ViewLayout/ContainerScope.hpp"
#include "Layout/LayoutHelpers.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace lambdaui {

namespace {

Size measureChild(Element const& child, MeasureContext& ctx, LayoutConstraints constraints,
                  LayoutHints hints, TextSystem& textSystem,
                  bool hasAssignedWidth = false,
                  bool hasAssignedHeight = false) {
  ctx.pushConstraints(constraints, hints, hasAssignedWidth, hasAssignedHeight);
  Size size = child.measure(ctx, constraints, hints, textSystem);
  ctx.popConstraints();
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

bool fixedFiniteWidth(LayoutConstraints const& constraints) {
  return positiveFinite(constraints.maxWidth) && constraints.minWidth >= constraints.maxWidth - 1e-4f;
}

bool finiteHeightIsConstrained(LayoutConstraints const& constraints, LayoutHints const& hints) {
  return axisStretches(hints.zStackVerticalAlign) ||
         (!hints.zStackVerticalAlign.has_value() && fixedFiniteHeight(constraints));
}

bool mainAxisExtentIsDefinite(layout::StackAxis axis,
                              MeasureContext const& ctx,
                              LayoutConstraints const& constraints,
                              LayoutHints const& hints) {
  // Main-axis flex follows the traversal assigned-size flag; tight finite and ZStack stretch
  // keep direct measurement and retained relayout paths equivalent.
  bool const frameAssigned = axis == layout::StackAxis::Horizontal
                                 ? ctx.hasAssignedWidth()
                                 : ctx.hasAssignedHeight();
  bool const tightFinite = axis == layout::StackAxis::Horizontal
                               ? fixedFiniteWidth(constraints)
                               : fixedFiniteHeight(constraints);
  bool const zStackStretch = axis == layout::StackAxis::Horizontal
                                 ? axisStretches(hints.zStackHorizontalAlign)
                                 : axisStretches(hints.zStackVerticalAlign);
  return frameAssigned || tightFinite || zStackStretch;
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

void rewindMeasuredChildren(MeasureContext& ctx) {
  ctx.rewindChildKeyIndex();
}

bool sameFlexMainSize(float a, float b) {
  return std::fabs(a - b) <= layout::kFlexEpsilon;
}

} // namespace

Size VStack::measure(MeasureContext& ctx, LayoutConstraints const& constraints,
                     LayoutHints const& hints, TextSystem& textSystem) const {
  ContainerMeasureScope scope(ctx);

  float const assignedWidth = finiteSpan(constraints.maxWidth);
  bool const widthAssigned = assignedWidth > 0.f && finiteWidthIsAssigned(hints);
  float const assignedHeight = finiteSpan(constraints.maxHeight);
  bool const heightConstrained = assignedHeight > 0.f &&
                                 mainAxisExtentIsDefinite(layout::StackAxis::Vertical,
                                                          ctx, constraints, hints);

  LayoutConstraints childConstraints = constraints;
  childConstraints.minWidth = 0.f;
  childConstraints.minHeight = 0.f;
  childConstraints.maxWidth = assignedWidth > 0.f
                                  ? assignedWidth
                                  : std::numeric_limits<float>::infinity();
  childConstraints.maxHeight = std::numeric_limits<float>::infinity();
  layout::clampLayoutMinToMax(childConstraints);

  LayoutHints childHints{};
  childHints.vStackCrossAlign = alignment;
  std::vector<Size> sizes;
  sizes.reserve(children.size());
  // Vertical children receive their final cross width up front, so their heights are stable in one pass.
  for (std::size_t i = 0; i < children.size(); ++i) {
    sizes.push_back(measureChild(children[i], ctx, childConstraints, childHints, textSystem,
                                 widthAssigned, false));
  }
  std::vector<std::size_t> const activeIndices = activeStackIndices(children, sizes);
  std::vector<Size> const activeSizes = sizesForIndices(sizes, activeIndices);

  std::vector<layout::StackMainAxisChild> stackChildren =
      stackChildrenForAxis(children, sizes, activeIndices, layout::StackAxis::Vertical);
  layout::StackMainAxisLayout const mainLayout =
      layout::layoutStackMainAxis(stackChildren, spacing, assignedHeight, heightConstrained,
                                  justifyContent);
  layout::StackLayoutResult layoutResult =
      layout::layoutStack(layout::StackAxis::Vertical, alignment, activeSizes, mainLayout.mainSizes,
                          mainLayout.itemSpacing, mainLayout.containerMainSize,
                          mainLayout.startOffset, assignedWidth, widthAssigned);
  layoutResult.containerSize.width = std::max(layoutResult.containerSize.width, constraints.minWidth);
  layoutResult.containerSize.height = std::max(layoutResult.containerSize.height, constraints.minHeight);
  if (std::isfinite(constraints.maxWidth)) {
    layoutResult.containerSize.width = std::min(layoutResult.containerSize.width, constraints.maxWidth);
  }
  if (std::isfinite(constraints.maxHeight) && heightConstrained) {
    layoutResult.containerSize.height = std::min(layoutResult.containerSize.height, constraints.maxHeight);
  }
  return layoutResult.containerSize;
}

Size HStack::measure(MeasureContext& ctx, LayoutConstraints const& constraints,
                     LayoutHints const& hints, TextSystem& textSystem) const {
  ContainerMeasureScope scope(ctx);
  if (children.empty()) {
    return {0.f, 0.f};
  }

  float const assignedWidth = finiteSpan(constraints.maxWidth);
  bool const widthConstrained = assignedWidth > 0.f &&
                                mainAxisExtentIsDefinite(layout::StackAxis::Horizontal,
                                                         ctx, constraints, hints);
  float const assignedHeight = finiteSpan(constraints.maxHeight);
  bool const heightConstrained = assignedHeight > 0.f && finiteHeightIsConstrained(constraints, hints);
  bool const stretchCrossAxis = alignment == Alignment::Stretch && heightConstrained;

  LayoutConstraints childConstraints = constraints;
  childConstraints.minWidth = 0.f;
  childConstraints.minHeight = 0.f;
  childConstraints.maxWidth = std::numeric_limits<float>::infinity();
  childConstraints.maxHeight = stretchCrossAxis ? assignedHeight
                                                : std::numeric_limits<float>::infinity();
  layout::clampLayoutMinToMax(childConstraints);

  std::vector<Size> initialSizes;
  initialSizes.reserve(children.size());
  for (std::size_t i = 0; i < children.size(); ++i) {
    initialSizes.push_back(measureChild(children[i], ctx, childConstraints, LayoutHints{}, textSystem,
                                        false, stretchCrossAxis));
  }

  std::vector<std::size_t> const activeIndices = activeStackIndices(children, initialSizes);
  std::vector<layout::StackMainAxisChild> stackChildren =
      stackChildrenForAxis(children, initialSizes, activeIndices,
                           layout::StackAxis::Horizontal);
  layout::StackMainAxisLayout const mainLayout =
      layout::layoutStackMainAxis(stackChildren, spacing, assignedWidth, widthConstrained,
                                  justifyContent);

  LayoutHints rowHints{};
  rowHints.hStackCrossAlign = alignment;
  std::vector<Size> rowSizes;
  rowSizes.reserve(activeIndices.size());
  float rowInnerHeight = 0.f;

  // Horizontal child height can depend on final width; remeasure only children whose width changed.
  bool needsSecondPass = false;
  for (std::size_t layoutIndex = 0; layoutIndex < activeIndices.size(); ++layoutIndex) {
    std::size_t const childIndex = activeIndices[layoutIndex];
    float const assignedChildWidth = layoutIndex < mainLayout.mainSizes.size()
                                         ? mainLayout.mainSizes[layoutIndex]
                                         : initialSizes[childIndex].width;
    if (!sameFlexMainSize(assignedChildWidth, initialSizes[childIndex].width)) {
      needsSecondPass = true;
      break;
    }
  }

  if (!needsSecondPass) {
    for (std::size_t const childIndex : activeIndices) {
      rowSizes.push_back(initialSizes[childIndex]);
      rowInnerHeight = std::max(rowInnerHeight, initialSizes[childIndex].height);
    }
  } else {
    rewindMeasuredChildren(ctx);
    for (std::size_t layoutIndex = 0; layoutIndex < activeIndices.size(); ++layoutIndex) {
      std::size_t const childIndex = activeIndices[layoutIndex];
      float const assignedChildWidth = layoutIndex < mainLayout.mainSizes.size()
                                           ? mainLayout.mainSizes[layoutIndex]
                                           : initialSizes[childIndex].width;
      if (sameFlexMainSize(assignedChildWidth, initialSizes[childIndex].width)) {
        rowSizes.push_back(initialSizes[childIndex]);
        rowInnerHeight = std::max(rowInnerHeight, initialSizes[childIndex].height);
        ctx.advanceChildSlot();
        continue;
      }

      LayoutConstraints childMeasure = constraints;
      childMeasure.minWidth = 0.f;
      childMeasure.minHeight = 0.f;
      childMeasure.maxWidth = assignedChildWidth;
      childMeasure.maxHeight = stretchCrossAxis ? assignedHeight
                                                : std::numeric_limits<float>::infinity();
      layout::clampLayoutMinToMax(childMeasure);
      Size const size = measureChild(children[childIndex], ctx, childMeasure, rowHints, textSystem,
                                     true, stretchCrossAxis);
      rowSizes.push_back(size);
      rowInnerHeight = std::max(rowInnerHeight, size.height);
    }
  }

  float const rowCrossSize = heightConstrained ? assignedHeight : rowInnerHeight;
  layout::StackLayoutResult layoutResult =
      layout::layoutStack(layout::StackAxis::Horizontal, alignment, rowSizes, mainLayout.mainSizes,
                          mainLayout.itemSpacing, mainLayout.containerMainSize,
                          mainLayout.startOffset, rowCrossSize, heightConstrained);
  layoutResult.containerSize.width = std::max(layoutResult.containerSize.width, constraints.minWidth);
  layoutResult.containerSize.height = std::max(layoutResult.containerSize.height, constraints.minHeight);
  if (std::isfinite(constraints.maxWidth) && widthConstrained) {
    layoutResult.containerSize.width = std::min(layoutResult.containerSize.width, constraints.maxWidth);
  }
  if (std::isfinite(constraints.maxHeight) && heightConstrained) {
    layoutResult.containerSize.height = std::min(layoutResult.containerSize.height, constraints.maxHeight);
  }
  return layoutResult.containerSize;
}

Size ZStack::measure(MeasureContext& ctx, LayoutConstraints const& constraints,
                     LayoutHints const&, TextSystem& textSystem) const {
  ContainerMeasureScope scope(ctx);
  float const assignedWidth = finiteSpan(constraints.maxWidth);
  float const assignedHeight = finiteSpan(constraints.maxHeight);
  float width = assignedWidth;
  float height = assignedHeight;

  LayoutConstraints childConstraints = constraints;
  childConstraints.minWidth = 0.f;
  childConstraints.minHeight = 0.f;
  childConstraints.maxWidth = assignedWidth > 0.f ? assignedWidth : std::numeric_limits<float>::infinity();
  childConstraints.maxHeight = assignedHeight > 0.f ? assignedHeight : std::numeric_limits<float>::infinity();
  layout::clampLayoutMinToMax(childConstraints);

  LayoutHints childHints{};
  childHints.zStackHorizontalAlign = horizontalAlignment;
  childHints.zStackVerticalAlign = verticalAlignment;
  for (Element const& child : children) {
    Size const childSize = measureChild(child, ctx, childConstraints, childHints, textSystem,
                                        assignedWidth > 0.f &&
                                            horizontalAlignment == Alignment::Stretch,
                                        assignedHeight > 0.f &&
                                            verticalAlignment == Alignment::Stretch);
    width = std::max(width, childSize.width);
    height = std::max(height, childSize.height);
  }
  width = std::max(width, constraints.minWidth);
  height = std::max(height, constraints.minHeight);
  if (std::isfinite(constraints.maxWidth)) {
    width = std::min(width, constraints.maxWidth);
  }
  if (std::isfinite(constraints.maxHeight)) {
    height = std::min(height, constraints.maxHeight);
  }
  return Size{width, height};
}

} // namespace lambdaui

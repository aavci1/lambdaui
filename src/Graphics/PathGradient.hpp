#pragma once

#include "Graphics/PathFlattener.hpp"

#include <algorithm>
#include <cmath>

namespace lambdaui {

inline Point pathGradientUnitPoint(Point point, Rect bounds) noexcept {
  float const invW = 1.f / std::max(bounds.width, 1e-4f);
  float const invH = 1.f / std::max(bounds.height, 1e-4f);
  return Point{(point.x - bounds.x) * invW, (point.y - bounds.y) * invH};
}

inline Color interpolatePathGradientStops(GradientStop const* stops, std::uint8_t stopCount,
                                          float t, float opacity) noexcept {
  t = std::clamp(t, 0.f, 1.f);
  if (stopCount == 0) {
    return Colors::transparent;
  }
  if (stopCount == 1 || t <= stops[0].position) {
    Color c = stops[0].color;
    c.a *= opacity;
    return c;
  }
  for (std::uint8_t i = 0; i + 1 < stopCount; ++i) {
    GradientStop const& a = stops[i];
    GradientStop const& b = stops[i + 1];
    if (t <= b.position || i + 2 == stopCount) {
      float const span = std::max(b.position - a.position, 1e-5f);
      float const u = std::clamp((t - a.position) / span, 0.f, 1.f);
      return Color{
          a.color.r + (b.color.r - a.color.r) * u,
          a.color.g + (b.color.g - a.color.g) * u,
          a.color.b + (b.color.b - a.color.b) * u,
          (a.color.a + (b.color.a - a.color.a) * u) * opacity,
      };
    }
  }
  Color c = stops[stopCount - 1].color;
  c.a *= opacity;
  return c;
}

inline bool pathGradientColorAt(FillStyle const& fill, Point unit, float opacity, Color* out) noexcept {
  LinearGradient linear{};
  if (fill.linearGradient(&linear) && linear.stopCount >= 2) {
    Point const axis = linear.end - linear.start;
    float const axisLenSq = axis.x * axis.x + axis.y * axis.y;
    float const t = axisLenSq > 1e-8f
                        ? ((unit.x - linear.start.x) * axis.x + (unit.y - linear.start.y) * axis.y) / axisLenSq
                        : 0.f;
    *out = interpolatePathGradientStops(linear.stops.data(), linear.stopCount, t, opacity);
    return true;
  }
  RadialGradient radial{};
  if (fill.radialGradient(&radial) && radial.stopCount >= 2) {
    float const dx = unit.x - radial.center.x;
    float const dy = unit.y - radial.center.y;
    float const t = std::sqrt(dx * dx + dy * dy) / std::max(radial.radius, 1e-4f);
    *out = interpolatePathGradientStops(radial.stops.data(), radial.stopCount, t, opacity);
    return true;
  }
  ConicalGradient conical{};
  if (fill.conicalGradient(&conical) && conical.stopCount >= 2) {
    constexpr float twoPi = 6.28318530718f;
    float const angle = std::atan2(unit.y - conical.center.y, unit.x - conical.center.x) -
                        conical.startAngleRadians;
    float t = angle / twoPi;
    t -= std::floor(t);
    *out = interpolatePathGradientStops(conical.stops.data(), conical.stopCount, t, opacity);
    return true;
  }
  return false;
}

inline bool applyPathGradientFill(TessellatedPath& tessellated, FillStyle const& fill,
                                  Rect bounds, float opacity) noexcept {
  if (tessellated.vertices.empty()) {
    return false;
  }
  for (PathVertex& vertex : tessellated.vertices) {
    Color color{};
    if (!pathGradientColorAt(fill, pathGradientUnitPoint(Point{vertex.x, vertex.y}, bounds), opacity, &color)) {
      return false;
    }
    vertex.color[0] = color.r;
    vertex.color[1] = color.g;
    vertex.color[2] = color.b;
    vertex.color[3] = color.a;
  }
  return true;
}

} // namespace lambdaui

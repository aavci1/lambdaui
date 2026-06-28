#include "Graphics/Metal/MetalPathRasterizer.hpp"
#include "Debug/PerfCounters.hpp"
#include "Graphics/PathGradient.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace lambdaui {

namespace {

template <typename Vec>
void recordCapacityIncrease(std::size_t previousCapacity, Vec const& vec) {
  if (!debug::perf::enabled() || vec.capacity() <= previousCapacity) {
    return;
  }
  using Value = typename Vec::value_type;
  debug::perf::recordRecorderCapacityGrowth(
      static_cast<std::uint64_t>((vec.capacity() - previousCapacity) * sizeof(Value)));
}

bool representativeFillColor(FillStyle const& fill, Color* out) {
  if (fill.solidColor(out)) {
    return true;
  }
  LinearGradient gradient{};
  if (fill.linearGradient(&gradient) && gradient.stopCount > 0) {
    *out = gradient.stops[0].color;
    return true;
  }
  RadialGradient radial{};
  if (fill.radialGradient(&radial) && radial.stopCount > 0) {
    *out = radial.stops[0].color;
    return true;
  }
  ConicalGradient conical{};
  if (fill.conicalGradient(&conical) && conical.stopCount > 0) {
    *out = conical.stops[0].color;
    return true;
  }
  return false;
}

Rect boundsOfSubpaths(std::vector<std::vector<Point>> const& subpaths) {
  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  for (auto const& subpath : subpaths) {
    for (Point const& point : subpath) {
      minX = std::min(minX, point.x);
      minY = std::min(minY, point.y);
      maxX = std::max(maxX, point.x);
      maxY = std::max(maxY, point.y);
    }
  }
  if (!std::isfinite(minX) || maxX <= minX || maxY <= minY) {
    return Rect::sharp(0.f, 0.f, 1.f, 1.f);
  }
  return Rect::sharp(minX, minY, maxX - minX, maxY - minY);
}

void applyGradientFill(TessellatedPath& tessellated, FillStyle const& fill, Rect bounds, float opacity) {
  (void)applyPathGradientFill(tessellated, fill, bounds, opacity);
}

} // namespace

void metalPathRasterizeToMesh(Path const& path, FillStyle const& fs, StrokeStyle const& ss, Mat3 const& transform,
                              float dpiScaleX, float dpiScaleY, float opacity, float viewportW, float viewportH,
                              std::vector<PathVertex>& pathVerts, std::vector<MetalPathOp>& pathOps,
                              std::vector<MetalOpRef>& opOrder, BlendMode blendMode) {
  if (path.isEmpty() || viewportW < 1.f || viewportH < 1.f) {
    return;
  }

  const float s = std::min(dpiScaleX, dpiScaleY);
  const size_t pathBegin = pathVerts.size();

  auto subpaths = PathFlattener::flattenSubpaths(path);
  for (auto& sp : subpaths) {
    for (auto& p : sp) {
      Point q = transform.apply(p);
      p = {q.x * dpiScaleX, q.y * dpiScaleY};
    }
  }
  Rect const fillBounds = boundsOfSubpaths(subpaths);

  auto appendVerts = [&pathVerts](TessellatedPath&& t) {
    if (t.vertices.empty()) {
      return;
    }
    std::size_t const previousCapacity = pathVerts.capacity();
    pathVerts.insert(pathVerts.end(), t.vertices.begin(), t.vertices.end());
    recordCapacityIncrease(previousCapacity, pathVerts);
  };

  auto appendFillVerts = [&appendVerts, &fs, fillBounds, opacity](TessellatedPath&& t) {
    applyGradientFill(t, fs, fillBounds, opacity);
    appendVerts(std::move(t));
  };

  if (!fs.isNone()) {
    Color fc{};
    if (representativeFillColor(fs, &fc)) {
      fc.a *= opacity;
      if (subpaths.size() > 1) {
        std::vector<std::vector<Point>> nonempty;
        nonempty.reserve(subpaths.size());
        for (const auto& s : subpaths) {
          if (s.size() >= 3) {
            nonempty.push_back(s);
          }
        }
        if (!nonempty.empty()) {
          appendFillVerts(PathFlattener::tessellateFillContours(
              nonempty, fc, viewportW, viewportH, PathFlattener::tessWindingFromFillRule(fs.fillRule)));
        }
      } else {
        for (auto& s : subpaths) {
          if (s.size() >= 3) {
            appendFillVerts(PathFlattener::tessellateFill(s, fc, viewportW, viewportH));
          }
        }
      }
    }
  }

  if (!ss.isNone()) {
    Color sc{};
    if (ss.solidColor(&sc)) {
      sc.a *= opacity;
      const float sw = ss.width * s;
      for (const auto& sp : subpaths) {
        if (sp.size() >= 2) {
          appendVerts(PathFlattener::tessellateStroke(sp, sw, sc, viewportW, viewportH, ss.join, ss.cap));
        }
      }
    }
  }

  const size_t pathEnd = pathVerts.size();
  if (pathEnd > pathBegin) {
    MetalPathOp pop{};
    pop.pathStart = static_cast<std::uint32_t>(pathBegin);
    pop.pathCount = static_cast<std::uint32_t>(pathEnd - pathBegin);
    pop.blendMode = blendMode;
    std::size_t const orderCapacity = opOrder.capacity();
    std::size_t const pathOpCapacity = pathOps.capacity();
    opOrder.push_back(MetalOpRef{
        .kind = MetalOpRef::Path,
        .index = static_cast<std::uint32_t>(pathOps.size()),
    });
    pathOps.push_back(pop);
    recordCapacityIncrease(orderCapacity, opOrder);
    recordCapacityIncrease(pathOpCapacity, pathOps);
  }
}

} // namespace lambdaui

#include <doctest/doctest.h>

#include "Graphics/PathGradient.hpp"

namespace {

void checkColor(lambdaui::Color color, float r, float g, float b, float a) {
  CHECK(color.r == doctest::Approx(r));
  CHECK(color.g == doctest::Approx(g));
  CHECK(color.b == doctest::Approx(b));
  CHECK(color.a == doctest::Approx(a));
}

} // namespace

TEST_CASE("PathGradient maps points into fill bounds") {
  lambdaui::Point unit = lambdaui::pathGradientUnitPoint({15.f, 25.f}, lambdaui::Rect::sharp(10.f, 20.f, 20.f, 10.f));
  CHECK(unit.x == doctest::Approx(0.25f));
  CHECK(unit.y == doctest::Approx(0.5f));
}

TEST_CASE("PathGradient samples linear gradient stops with opacity") {
  using namespace lambdaui;
  FillStyle fill = FillStyle::linearGradient(
      {GradientStop{0.f, Color{1.f, 0.f, 0.f, 1.f}}, GradientStop{1.f, Color{0.f, 0.f, 1.f, 0.5f}}},
      {0.f, 0.f}, {1.f, 0.f});

  Color color{};
  REQUIRE(pathGradientColorAt(fill, {0.5f, 0.f}, 0.5f, &color));
  checkColor(color, 0.5f, 0.f, 0.5f, 0.375f);
}

TEST_CASE("PathGradient applies sampled colors to tessellated vertices") {
  using namespace lambdaui;
  FillStyle fill = FillStyle::linearGradient(
      {GradientStop{0.f, Color{0.f, 0.f, 0.f, 1.f}}, GradientStop{1.f, Color{1.f, 1.f, 1.f, 1.f}}},
      {0.f, 0.f}, {1.f, 0.f});
  TessellatedPath path{};
  path.vertices.push_back(PathVertex{.x = 0.f, .y = 0.f});
  path.vertices.push_back(PathVertex{.x = 10.f, .y = 0.f});

  CHECK(applyPathGradientFill(path, fill, Rect::sharp(0.f, 0.f, 10.f, 10.f), 1.f));
  CHECK(path.vertices[0].color[0] == doctest::Approx(0.f));
  CHECK(path.vertices[1].color[0] == doctest::Approx(1.f));
}

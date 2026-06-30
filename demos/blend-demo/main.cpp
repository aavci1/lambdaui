#include <Lambda.hpp>
#include <Lambda/UI/Application.hpp>
#include <Lambda/UI/WindowUI.hpp>
#include <Lambda/Graphics/TextSystem.hpp>
#include <Lambda/UI/Theme.hpp>
#include <Lambda/UI/UI.hpp>
#include <Lambda/UI/ViewModifiers.hpp>
#include <Lambda/UI/Views/Grid.hpp>
#include <Lambda/UI/Views/Render.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace lambdaui;

namespace {

struct DemoCell {
  BlendMode mode;
  const char* title;
};

// Row-major grid: each cell shows cyan + magenta circles; second circle uses `mode`.
constexpr std::array<DemoCell, 12> kGrid{{
    {BlendMode::Normal, "Normal"},
    {BlendMode::Multiply, "Multiply"},
    {BlendMode::Screen, "Screen"},
    {BlendMode::Darken, "Darken"},
    {BlendMode::Lighten, "Lighten"},
    {BlendMode::DstOver, "DstOver"},
    {BlendMode::SrcIn, "SrcIn"},
    {BlendMode::DstIn, "DstIn"},
    {BlendMode::SrcOut, "SrcOut"},
    {BlendMode::DstOut, "DstOut"},
    {BlendMode::Src, "Src"},
    {BlendMode::Dst, "Dst"},
}};

constexpr Color kDestinationColor{0.05f, 0.75f, 0.85f, 1.f};
constexpr Color kSourceColor{0.92f, 0.15f, 0.55f, 1.f};

struct PreviewGeometry {
  Point destinationCenter{};
  Point sourceCenter{};
  float radius = 0.f;
};

struct PremultipliedColor {
  float r = 0.f;
  float g = 0.f;
  float b = 0.f;
  float a = 0.f;
};

float clamp01(float value) {
  return std::clamp(value, 0.f, 1.f);
}

std::uint8_t toByte(float value) {
  return static_cast<std::uint8_t>(std::lround(clamp01(value) * 255.f));
}

PreviewGeometry previewGeometry(Size size) {
  return PreviewGeometry{
      .destinationCenter = Point{size.width * 0.34f, size.height * 0.45f},
      .sourceCenter = Point{size.width * 0.66f, size.height * 0.45f},
      .radius = std::min(size.width, size.height) * 0.24f,
  };
}

float circleCoverage(Point point, Point center, float radius, float pixelScale) {
  float const dx = point.x - center.x;
  float const dy = point.y - center.y;
  float const distance = std::sqrt(dx * dx + dy * dy);
  return clamp01((radius - distance) * pixelScale + 0.5f);
}

PremultipliedColor premultiply(Color color, float coverage) {
  float const alpha = clamp01(color.a * coverage);
  return PremultipliedColor{
      .r = color.r * alpha,
      .g = color.g * alpha,
      .b = color.b * alpha,
      .a = alpha,
  };
}

float blendChannel(float source, float destination, BlendMode mode) {
  switch (mode) {
  case BlendMode::Multiply:
    return source * destination;
  case BlendMode::Screen:
    return source + destination - source * destination;
  case BlendMode::Darken:
    return std::min(source, destination);
  case BlendMode::Lighten:
    return std::max(source, destination);
  default:
    return source;
  }
}

PremultipliedColor blendSeparable(Color source, float sourceAlpha,
                                  Color destination, float destinationAlpha,
                                  BlendMode mode) {
  float const sourceOnly = sourceAlpha * (1.f - destinationAlpha);
  float const destinationOnly = destinationAlpha * (1.f - sourceAlpha);
  float const overlap = sourceAlpha * destinationAlpha;
  return PremultipliedColor{
      .r = source.r * sourceOnly + destination.r * destinationOnly +
           blendChannel(source.r, destination.r, mode) * overlap,
      .g = source.g * sourceOnly + destination.g * destinationOnly +
           blendChannel(source.g, destination.g, mode) * overlap,
      .b = source.b * sourceOnly + destination.b * destinationOnly +
           blendChannel(source.b, destination.b, mode) * overlap,
      .a = sourceAlpha + destinationAlpha - sourceAlpha * destinationAlpha,
  };
}

PremultipliedColor blendPixel(BlendMode mode, float sourceCoverage, float destinationCoverage) {
  PremultipliedColor const source = premultiply(kSourceColor, sourceCoverage);
  PremultipliedColor const destination = premultiply(kDestinationColor, destinationCoverage);
  float const sourceAlpha = source.a;
  float const destinationAlpha = destination.a;

  switch (mode) {
  case BlendMode::Multiply:
  case BlendMode::Screen:
  case BlendMode::Darken:
  case BlendMode::Lighten:
    return blendSeparable(kSourceColor, sourceAlpha, kDestinationColor, destinationAlpha, mode);
  case BlendMode::Clear:
    return {};
  case BlendMode::Src:
    return source;
  case BlendMode::Dst:
    return destination;
  case BlendMode::DstOver:
    return PremultipliedColor{
        .r = destination.r + source.r * (1.f - destinationAlpha),
        .g = destination.g + source.g * (1.f - destinationAlpha),
        .b = destination.b + source.b * (1.f - destinationAlpha),
        .a = destinationAlpha + sourceAlpha * (1.f - destinationAlpha),
    };
  case BlendMode::SrcIn:
    return PremultipliedColor{
        .r = source.r * destinationAlpha,
        .g = source.g * destinationAlpha,
        .b = source.b * destinationAlpha,
        .a = sourceAlpha * destinationAlpha,
    };
  case BlendMode::DstIn:
    return PremultipliedColor{
        .r = destination.r * sourceAlpha,
        .g = destination.g * sourceAlpha,
        .b = destination.b * sourceAlpha,
        .a = destinationAlpha * sourceAlpha,
    };
  case BlendMode::SrcOut:
    return PremultipliedColor{
        .r = source.r * (1.f - destinationAlpha),
        .g = source.g * (1.f - destinationAlpha),
        .b = source.b * (1.f - destinationAlpha),
        .a = sourceAlpha * (1.f - destinationAlpha),
    };
  case BlendMode::DstOut:
    return PremultipliedColor{
        .r = destination.r * (1.f - sourceAlpha),
        .g = destination.g * (1.f - sourceAlpha),
        .b = destination.b * (1.f - sourceAlpha),
        .a = destinationAlpha * (1.f - sourceAlpha),
    };
  case BlendMode::SrcAtop:
    return PremultipliedColor{
        .r = source.r * destinationAlpha + destination.r * (1.f - sourceAlpha),
        .g = source.g * destinationAlpha + destination.g * (1.f - sourceAlpha),
        .b = source.b * destinationAlpha + destination.b * (1.f - sourceAlpha),
        .a = destinationAlpha,
    };
  case BlendMode::DstAtop:
    return PremultipliedColor{
        .r = destination.r * sourceAlpha + source.r * (1.f - destinationAlpha),
        .g = destination.g * sourceAlpha + source.g * (1.f - destinationAlpha),
        .b = destination.b * sourceAlpha + source.b * (1.f - destinationAlpha),
        .a = sourceAlpha,
    };
  case BlendMode::Xor:
    return PremultipliedColor{
        .r = source.r * (1.f - destinationAlpha) + destination.r * (1.f - sourceAlpha),
        .g = source.g * (1.f - destinationAlpha) + destination.g * (1.f - sourceAlpha),
        .b = source.b * (1.f - destinationAlpha) + destination.b * (1.f - sourceAlpha),
        .a = sourceAlpha * (1.f - destinationAlpha) + destinationAlpha * (1.f - sourceAlpha),
    };
  case BlendMode::Normal:
  case BlendMode::SrcOver:
  default:
    return PremultipliedColor{
        .r = source.r + destination.r * (1.f - sourceAlpha),
        .g = source.g + destination.g * (1.f - sourceAlpha),
        .b = source.b + destination.b * (1.f - sourceAlpha),
        .a = sourceAlpha + destinationAlpha * (1.f - sourceAlpha),
    };
  }
}

std::shared_ptr<Image> makeBlendPreviewImage(Canvas& canvas, Size logicalSize, BlendMode mode) {
  float const dpiScale = std::max(canvas.dpiScale(), 1.f);
  auto const width = static_cast<std::uint32_t>(std::max(1.f, std::ceil(logicalSize.width * dpiScale)));
  auto const height = static_cast<std::uint32_t>(std::max(1.f, std::ceil(logicalSize.height * dpiScale)));
  float const scaleX = static_cast<float>(width) / std::max(logicalSize.width, 1.f);
  float const scaleY = static_cast<float>(height) / std::max(logicalSize.height, 1.f);
  float const pixelScale = std::min(scaleX, scaleY);
  PreviewGeometry const geometry = previewGeometry(logicalSize);

  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u, 0);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      Point const point{
          (static_cast<float>(x) + 0.5f) / scaleX,
          (static_cast<float>(y) + 0.5f) / scaleY,
      };
      float const destinationCoverage =
          circleCoverage(point, geometry.destinationCenter, geometry.radius, pixelScale);
      float const sourceCoverage =
          circleCoverage(point, geometry.sourceCenter, geometry.radius, pixelScale);
      PremultipliedColor const blended = blendPixel(mode, sourceCoverage, destinationCoverage);
      std::size_t const offset = (static_cast<std::size_t>(y) * width + x) * 4u;
      if (blended.a <= 0.0001f) {
        continue;
      }
      pixels[offset + 0u] = toByte(blended.r / blended.a);
      pixels[offset + 1u] = toByte(blended.g / blended.a);
      pixels[offset + 2u] = toByte(blended.b / blended.a);
      pixels[offset + 3u] = toByte(blended.a);
    }
  }

  return Image::fromRgbaPixels(width, height, pixels, canvas.webGpuDevice(), canvas.webGpuQueue());
}

struct BlendCell : ViewModifiers<BlendCell> {
  static constexpr float kMargin = 24.f;
  static constexpr float kGap = 10.f;

  BlendMode mode = BlendMode::Normal;
  const char* title = "";

  auto body() const {
    BlendMode const cellMode = mode;
    std::string const label = title ? title : "";
    return Render{
        .measureFn =
            [](LayoutConstraints const& c, LayoutHints const&) {
              float const w = std::isfinite(c.maxWidth) && c.maxWidth > 0.f ? c.maxWidth : 0.f;
              float const h = std::isfinite(c.maxHeight) && c.maxHeight > 0.f
                                  ? c.maxHeight
                                  : std::max(112.f, w * 0.72f);
              return Size{w, h};
            },
        .draw =
            [cellMode, label](Canvas& canvas, Rect cell) {
              float const pad = 8.f;
              float const innerX = cell.x + pad;
              float const innerY = cell.y + pad;
              float const innerW = std::max(cell.width - pad * 2.f, 4.f);
              float const innerH = std::max(cell.height - pad * 2.f, 4.f);
              Rect const inner = Rect::sharp(innerX, innerY, innerW, innerH);

              canvas.setBlendMode(BlendMode::Normal);
              canvas.drawRect(inner, {}, FillStyle::solid(Color::rgb(210, 210, 218)), StrokeStyle::none());

              std::shared_ptr<Image> blendLayer =
                  makeBlendPreviewImage(canvas, Size{innerW, innerH}, cellMode);
              if (blendLayer) {
                canvas.drawImage(*blendLayer, inner, ImageFillMode::Stretch);
              }

              canvas.setBlendMode(BlendMode::Normal);
              canvas.drawRect(cell, CornerRadius(4.f, 4.f, 4.f, 4.f), FillStyle::none(),
                              StrokeStyle::solid(Color::rgb(90, 90, 100), 1.2f));

              Theme const theme = Theme::light();
              Font const labelFont = theme.headlineFont;
              auto labelLayout =
                  Application::instance().textSystem().layout(label, labelFont, theme.labelColor, 0.f);
              Size const m = labelLayout->measuredSize;
              float const y = cell.y + cell.height - 16.f - m.height;
              float const x = cell.x + (cell.width - m.width) * 0.5f;
              canvas.drawTextLayout(*labelLayout, Point{x, y});
        },
    };
  }
};

struct BlendDemoView {
  auto body() const {
    std::size_t constexpr columns = 4u;
    std::vector<Element> cells;
    cells.reserve(kGrid.size());
    for (DemoCell const& d : kGrid) {
      cells.push_back(Element{BlendCell{.mode = d.mode, .title = d.title}});
    }
    return Grid{
                    .columns = columns,
                    .horizontalSpacing = BlendCell::kGap,
                    .verticalSpacing = BlendCell::kGap,
                    .horizontalAlignment = Alignment::Stretch,
                    .verticalAlignment = Alignment::Stretch,
                    .children = std::move(cells),
    }.padding(BlendCell::kMargin);
  }
};

} // namespace

int main(int argc, char* argv[]) {
  Application app(argc, argv);

  app.createWindow<Window>({
      .size = {800, 800},
      .title = "Lambda — blend modes",
  }).setView(BlendDemoView{});

  return app.exec();
}

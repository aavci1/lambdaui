#include "Graphics/Linux/FreeTypeTextSystem.hpp"

#include <Lambda/Graphics/AttributedString.hpp>
#include <Lambda/Graphics/TextCacheStats.hpp>
#include <Lambda/Graphics/TextLayoutOptions.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>

using namespace lambdaui;

namespace {

Font testFont() {
  Font font = Font::body();
  font.family = "sans-serif";
  font.size = 16.f;
  font.weight = 400.f;
  return font;
}

} // namespace

TEST_CASE("FreeTypeTextSystem reports line boxes from resolved line height") {
  FreeTypeTextSystem textSystem([] {
    return "lambda-tests";
  });

  TextLayoutOptions options{};
  options.wrapping = TextWrapping::NoWrap;
  options.lineHeight = 30.f;

  auto layout = textSystem.layout("Agy", Font::body(), Colors::black, 0.f, options);
  REQUIRE(layout);
  REQUIRE(layout->lines.size() == 1);
  CHECK(layout->lines.front().top == doctest::Approx(0.f));
  CHECK(layout->lines.front().bottom == doctest::Approx(30.f));
  CHECK(layout->measuredSize.height == doctest::Approx(30.f));
  CHECK(layout->firstBaseline == doctest::Approx(layout->lines.front().baseline));
}

TEST_CASE("FreeTypeTextSystem centers boxed text using line box height") {
  FreeTypeTextSystem textSystem([] {
    return "lambda-tests";
  });

  TextLayoutOptions options{};
  options.wrapping = TextWrapping::NoWrap;
  options.verticalAlignment = VerticalAlignment::Center;
  options.lineHeight = 30.f;

  Rect const box = Rect::sharp(0.f, 0.f, 200.f, 50.f);
  TextSystem& boxedTextSystem = textSystem;
  auto layout = boxedTextSystem.layout("Agy", Font::body(), Colors::black, box, options);
  REQUIRE(layout);
  REQUIRE(layout->lines.size() == 1);
  CHECK(layout->lines.front().top == doctest::Approx(10.f));
  CHECK(layout->lines.front().bottom == doctest::Approx(40.f));
  CHECK(layout->measuredSize.height == doctest::Approx(30.f));
}

TEST_CASE("FreeTypeTextSystem content cache separates embedded key delimiter bytes") {
  FreeTypeTextSystem textSystem([] {
    return "lambda-tests";
  });

  TextLayoutOptions options{};
  options.wrapping = TextWrapping::NoWrap;

  std::string withDelimiters = "cache key";
  withDelimiters.push_back('\x1f');
  withDelimiters += "0.000000:0:0:0:0.000000:0.000000:0:0.000000";
  withDelimiters.push_back('\x1e');
  withDelimiters += "run";

  auto plain = textSystem.layout("cache key", testFont(), Colors::black, 0.f, options);
  auto delimited = textSystem.layout(withDelimiters, testFont(), Colors::black, 0.f, options);
  REQUIRE(plain);
  REQUIRE(delimited);
  CHECK(plain != delimited);
  CHECK(delimited->measuredSize.width > plain->measuredSize.width);

  auto plainAgain = textSystem.layout("cache key", testFont(), Colors::black, 0.f, options);
  auto delimitedAgain = textSystem.layout(withDelimiters, testFont(), Colors::black, 0.f, options);
  CHECK(plainAgain.get() == plain.get());
  CHECK(delimitedAgain.get() == delimited.get());

  TextCacheStats const stats = textSystem.stats();
  CHECK(stats.l3_layout.hits >= 2);
  CHECK(stats.l3_layout.misses >= 2);
}

TEST_CASE("FreeTypeTextSystem reuses long text layouts by fixed-size content key") {
  FreeTypeTextSystem textSystem([] {
    return "lambda-tests";
  });

  std::string longText;
  longText.reserve(4096);
  for (int i = 0; i < 96; ++i) {
    longText += "The cache key stores a fixed hash instead of the full paragraph body. ";
  }

  TextLayoutOptions options{};
  options.wrapping = TextWrapping::Wrap;
  auto first = textSystem.layout(longText, testFont(), Colors::black, 320.f, options);
  auto second = textSystem.layout(longText, testFont(), Colors::black, 320.f, options);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first.get() == second.get());

  TextCacheStats const stats = textSystem.stats();
  CHECK(stats.l3_layout.hits >= 1);
  CHECK(stats.l3_layout.currentBytes >= 1);
}

TEST_CASE("FreeTypeTextSystem caches boxed layouts by content box and placement options") {
  FreeTypeTextSystem textSystem([] {
    return "lambda-tests";
  });
  TextSystem& boxedTextSystem = textSystem;

  TextLayoutOptions options{};
  options.wrapping = TextWrapping::Wrap;
  options.horizontalAlignment = HorizontalAlignment::Center;
  options.verticalAlignment = VerticalAlignment::Center;
  options.lineHeight = 22.f;

  AttributedString text = AttributedString::plain("first line\nsecond line is wider",
                                                  testFont(),
                                                  Colors::black);
  Rect const box = Rect::sharp(4.f, 8.f, 220.f, 96.f);
  auto first = boxedTextSystem.layout(text, box, options);
  auto second = boxedTextSystem.layout(text, box, options);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first.get() == second.get());

  Rect const widerBox = Rect::sharp(4.f, 8.f, 260.f, 96.f);
  auto wider = boxedTextSystem.layout(text, widerBox, options);
  REQUIRE(wider);
  CHECK(wider.get() != first.get());

  TextLayoutOptions leadingOptions = options;
  leadingOptions.horizontalAlignment = HorizontalAlignment::Leading;
  auto leading = boxedTextSystem.layout(text, box, leadingOptions);
  REQUIRE(leading);
  CHECK(leading.get() != first.get());

  TextCacheStats beforeInvalidation = textSystem.stats();
  CHECK(beforeInvalidation.l4_boxLayout.hits >= 1);
  CHECK(beforeInvalidation.l4_boxLayout.misses >= 3);

  textSystem.invalidateAll();
  auto afterInvalidate = boxedTextSystem.layout(text, box, options);
  REQUIRE(afterInvalidate);
  CHECK(afterInvalidate.get() != first.get());

  std::array<std::uint32_t, 1> changedFontIds{0};
  textSystem.invalidateForFontChange(std::span<std::uint32_t const>(changedFontIds.data(),
                                                                    changedFontIds.size()));
  auto afterFontInvalidate = boxedTextSystem.layout(text, box, options);
  REQUIRE(afterFontInvalidate);
  CHECK(afterFontInvalidate.get() != afterInvalidate.get());
}

#include <doctest/doctest.h>

#include "Graphics/GlyphAtlasAllocator.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace {

lambdaui::GlyphAtlasKey key(std::uint32_t glyphId) {
  return lambdaui::GlyphAtlasKey{
      .fontId = 7,
      .glyphId = glyphId,
      .size = 12,
  };
}

} // namespace

TEST_CASE("GlyphAtlasAllocator wraps shelves with one-pixel padding") {
  lambdaui::GlyphAtlasAllocator allocator{16, 16};

  std::optional<lambdaui::GlyphAtlasPlacement> first = allocator.allocate(4, 4);
  REQUIRE(first);
  CHECK(first->x == 2);
  CHECK(first->y == 2);

  std::optional<lambdaui::GlyphAtlasPlacement> second = allocator.allocate(4, 4);
  REQUIRE(second);
  CHECK(second->x == 9);
  CHECK(second->y == 2);

  std::optional<lambdaui::GlyphAtlasPlacement> third = allocator.allocate(4, 4);
  REQUIRE(third);
  CHECK(third->x == 2);
  CHECK(third->y == 9);

  std::optional<lambdaui::GlyphAtlasPlacement> fourth = allocator.allocate(4, 4);
  REQUIRE(fourth);
  CHECK(fourth->x == 9);
  CHECK(fourth->y == 9);

  CHECK_FALSE(allocator.hasSpace(4, 4));
}

TEST_CASE("GlyphAtlasAllocator reports bounded growth for new glyphs") {
  lambdaui::GlyphAtlasAllocator allocator{16, 16, 64};
  while (allocator.allocate(4, 4)) {
  }

  std::optional<lambdaui::GlyphAtlasSize> grown = allocator.growthSizeFor(4, 4);
  REQUIRE(grown);
  CHECK(grown->width == 32);
  CHECK(grown->height == 32);

  allocator.resize(grown->width, grown->height);
  CHECK(allocator.hasSpace(4, 4));
}

TEST_CASE("GlyphAtlasAllocator evicts least-recently-used entries first") {
  lambdaui::GlyphAtlasAllocator allocator{64, 64};
  for (std::uint32_t glyphId = 1; glyphId <= 4; ++glyphId) {
    std::optional<lambdaui::GlyphAtlasPlacement> placement = allocator.allocate(4, 4);
    REQUIRE(placement);
    allocator.insert(key(glyphId), *placement, 4, 4, {});
  }

  (void)allocator.touch(key(1));
  (void)allocator.touch(key(2));

  std::vector<lambdaui::GlyphAtlasKey> evicted = allocator.oldestKeysToEvict();
  REQUIRE(evicted.size() == 1);
  CHECK(evicted.front() == key(3));
}

#include <doctest/doctest.h>

#include "Graphics/PathTessellationCache.hpp"

#include <vector>

namespace {

lambdaui::PathTessellationCacheKey key(std::uint64_t pathHash) {
  return lambdaui::PathTessellationCacheKey{
      .pathHash = pathHash,
      .styleHash = 7,
      .viewportW = 320,
      .viewportH = 240,
  };
}

} // namespace

TEST_CASE("PathTessellationCache returns cached vertices and tracks totals") {
  lambdaui::BasicPathTessellationCache<int> cache{16};

  CHECK(cache.find(key(1)) == nullptr);
  CHECK(cache.insert(key(1), std::vector<int>{1, 2, 3}));

  auto const* cached = cache.find(key(1));
  REQUIRE(cached != nullptr);
  CHECK(*cached == std::vector<int>{1, 2, 3});
  CHECK(cache.entryCount() == 1);
  CHECK(cache.cachedVertexCount() == 3);
}

TEST_CASE("PathTessellationCache evicts least recently used entries when over budget") {
  lambdaui::BasicPathTessellationCache<int> cache{4};

  CHECK(cache.insert(key(1), std::vector<int>{1, 1}));
  CHECK(cache.insert(key(2), std::vector<int>{2, 2}));
  REQUIRE(cache.find(key(1)) != nullptr);

  CHECK(cache.insert(key(3), std::vector<int>{3, 3}));
  CHECK(cache.find(key(2)) == nullptr);
  CHECK(cache.find(key(1)) != nullptr);
  CHECK(cache.find(key(3)) != nullptr);
  CHECK(cache.cachedVertexCount() == 4);
}

TEST_CASE("PathTessellationCache distinguishes style and viewport keys") {
  lambdaui::BasicPathTessellationCache<int> cache{16};
  auto base = key(1);
  auto styled = base;
  styled.styleHash = 8;
  auto resized = base;
  resized.viewportW = 640;

  CHECK(cache.insert(base, std::vector<int>{1}));
  CHECK(cache.insert(styled, std::vector<int>{2}));
  CHECK(cache.insert(resized, std::vector<int>{3}));

  REQUIRE(cache.find(base) != nullptr);
  CHECK(*cache.find(base) == std::vector<int>{1});
  REQUIRE(cache.find(styled) != nullptr);
  CHECK(*cache.find(styled) == std::vector<int>{2});
  REQUIRE(cache.find(resized) != nullptr);
  CHECK(*cache.find(resized) == std::vector<int>{3});
}

TEST_CASE("PathTessellationCache replacement updates vertex budget") {
  lambdaui::BasicPathTessellationCache<int> cache{4};

  CHECK(cache.insert(key(1), std::vector<int>{1, 2, 3}));
  CHECK_FALSE(cache.insert(key(1), std::vector<int>{4}));

  auto const* cached = cache.find(key(1));
  REQUIRE(cached != nullptr);
  CHECK(*cached == std::vector<int>{4});
  CHECK(cache.entryCount() == 1);
  CHECK(cache.cachedVertexCount() == 1);
}

TEST_CASE("PathTessellationCache style hash changes with paint transform and scale") {
  using namespace lambdaui;

  auto const fill = FillStyle::solid(Color{1.f, 0.f, 0.f, 1.f});
  auto const otherFill = FillStyle::solid(Color{0.f, 1.f, 0.f, 1.f});
  auto const stroke = StrokeStyle::none();
  Mat3 const identity = Mat3::identity();
  Mat3 const translated = Mat3::translate(4.f, 2.f);

  std::uint64_t const base = pathTessellationStyleHash(fill, stroke, identity, 1.f);
  CHECK(pathTessellationStyleHash(otherFill, stroke, identity, 1.f) != base);
  CHECK(pathTessellationStyleHash(fill, stroke, translated, 1.f) != base);
  CHECK(pathTessellationStyleHash(fill, stroke, identity, 0.5f) != base);
  CHECK(pathTessellationStyleHash(fill, stroke, identity, 1.f, pathTessellationScaleHash(2.f, 1.f)) != base);
}

#include <doctest/doctest.h>

#include "Graphics/BatchPredicates.hpp"

TEST_CASE("Batch predicates compare scissor validity and bounds") {
  using namespace lambdaui;

  CHECK(sameBatchScissor({}, {}));
  CHECK(sameBatchScissor(BatchScissor{.valid = true, .x = 1, .y = 2, .width = 3, .height = 4},
                         BatchScissor{.valid = true, .x = 1, .y = 2, .width = 3, .height = 4}));
  CHECK_FALSE(sameBatchScissor(BatchScissor{.valid = false},
                               BatchScissor{.valid = true, .x = 1, .y = 2, .width = 3, .height = 4}));
  CHECK_FALSE(sameBatchScissor(BatchScissor{.valid = true, .x = 1, .y = 2, .width = 3, .height = 4},
                               BatchScissor{.valid = true, .x = 1, .y = 2, .width = 4, .height = 4}));
}

TEST_CASE("Batch predicates compare rect and translation with caller-selected tolerance") {
  using namespace lambdaui;

  CHECK(sameBatchRect(Rect::sharp(0.f, 0.f, 10.f, 10.f), Rect::sharp(0.f, 0.f, 10.00001f, 10.f)));
  CHECK_FALSE(sameBatchRect(Rect::sharp(0.f, 0.f, 10.f, 10.f), Rect::sharp(0.f, 0.f, 10.01f, 10.f)));

  CHECK_FALSE(sameBatchTranslation(BatchTranslation{.x = 1.f, .y = 2.f},
                                   BatchTranslation{.x = 1.00001f, .y = 2.f}));
  CHECK(sameBatchTranslation(BatchTranslation{.x = 1.f, .y = 2.f},
                             BatchTranslation{.x = 1.00001f, .y = 2.f},
                             1e-4f));
}

TEST_CASE("Batch predicates compare opaque payload bytes") {
  std::uint32_t a[2] = {1, 2};
  std::uint32_t b[2] = {1, 2};
  std::uint32_t c[2] = {1, 3};

  CHECK(lambdaui::sameBatchPayloadBytes(a, b, sizeof(a)));
  CHECK_FALSE(lambdaui::sameBatchPayloadBytes(a, c, sizeof(a)));
}

#pragma once

#include <Lambda/Core/Geometry.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lambdaui {

struct BatchScissor {
  bool valid = false;
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct BatchTranslation {
  float x = 0.f;
  float y = 0.f;
};

inline bool sameBatchRect(Rect a, Rect b, float epsilon = 1e-4f) noexcept {
  return std::abs(a.x - b.x) <= epsilon &&
         std::abs(a.y - b.y) <= epsilon &&
         std::abs(a.width - b.width) <= epsilon &&
         std::abs(a.height - b.height) <= epsilon;
}

inline bool sameBatchScissor(BatchScissor a, BatchScissor b) noexcept {
  if (a.valid != b.valid) {
    return false;
  }
  if (!a.valid) {
    return true;
  }
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

inline bool sameBatchTranslation(BatchTranslation a, BatchTranslation b, float epsilon = 0.f) noexcept {
  return std::abs(a.x - b.x) <= epsilon && std::abs(a.y - b.y) <= epsilon;
}

inline bool sameBatchPayloadBytes(void const* a, void const* b, std::size_t size) noexcept {
  return std::memcmp(a, b, size) == 0;
}

} // namespace lambdaui

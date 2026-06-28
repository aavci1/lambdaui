#pragma once

#include "Graphics/PathFlattener.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lambdaui {

namespace path_tessellation_cache_detail {

inline std::uint64_t mixHashWord(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31u);
}

inline void hashBytes(std::uint64_t& h, void const* data, std::size_t size) noexcept {
  auto const* bytes = static_cast<unsigned char const*>(data);
  std::size_t i = 0;
  while (i + sizeof(std::uint64_t) <= size) {
    std::uint64_t word = 0;
    for (std::size_t b = 0; b < sizeof(std::uint64_t); ++b) {
      word |= static_cast<std::uint64_t>(bytes[i + b]) << (b * 8u);
    }
    h ^= mixHashWord(word);
    h *= 1099511628211ULL;
    i += sizeof(std::uint64_t);
  }
  if (i < size) {
    std::uint64_t tail = 0;
    for (std::size_t shift = 0; i < size; ++i, shift += 8) {
      tail |= static_cast<std::uint64_t>(bytes[i]) << shift;
    }
    h ^= mixHashWord(tail ^ size);
    h *= 1099511628211ULL;
  }
}

template <typename T>
inline void hashValue(std::uint64_t& h, T const& value) noexcept {
  hashBytes(h, &value, sizeof(value));
}

inline void hashColor(std::uint64_t& h, Color color) noexcept {
  hashValue(h, color.r);
  hashValue(h, color.g);
  hashValue(h, color.b);
  hashValue(h, color.a);
}

inline void hashPoint(std::uint64_t& h, Point point) noexcept {
  hashValue(h, point.x);
  hashValue(h, point.y);
}

inline void hashStops(std::uint64_t& h, std::array<GradientStop, kMaxGradientStops> const& stops,
                      std::uint8_t count) noexcept {
  hashValue(h, count);
  for (std::uint8_t i = 0; i < count; ++i) {
    hashValue(h, stops[i].position);
    hashColor(h, stops[i].color);
  }
}

} // namespace path_tessellation_cache_detail

inline std::uint64_t pathTessellationFillHash(FillStyle const& fill) noexcept {
  using namespace path_tessellation_cache_detail;
  std::uint64_t h = 14695981039346656037ULL;
  hashValue(h, fill.fillRule);
  hashValue(h, fill.data.index());
  Color solid{};
  if (fill.solidColor(&solid)) {
    hashColor(h, solid);
  }
  LinearGradient linear{};
  if (fill.linearGradient(&linear)) {
    hashPoint(h, linear.start);
    hashPoint(h, linear.end);
    hashStops(h, linear.stops, linear.stopCount);
  }
  RadialGradient radial{};
  if (fill.radialGradient(&radial)) {
    hashPoint(h, radial.center);
    hashValue(h, radial.radius);
    hashStops(h, radial.stops, radial.stopCount);
  }
  ConicalGradient conical{};
  if (fill.conicalGradient(&conical)) {
    hashPoint(h, conical.center);
    hashValue(h, conical.startAngleRadians);
    hashStops(h, conical.stops, conical.stopCount);
  }
  return h;
}

inline std::uint64_t pathTessellationStrokeHash(StrokeStyle const& stroke) noexcept {
  using namespace path_tessellation_cache_detail;
  std::uint64_t h = 14695981039346656037ULL;
  hashValue(h, stroke.type);
  hashColor(h, stroke.color);
  hashValue(h, stroke.width);
  hashValue(h, stroke.cap);
  hashValue(h, stroke.join);
  hashValue(h, stroke.miterLimit);
  return h;
}

inline std::uint64_t pathTessellationTransformHash(Mat3 const& transform, float opacity) noexcept {
  using namespace path_tessellation_cache_detail;
  std::uint64_t h = 14695981039346656037ULL;
  for (float value : transform.m) {
    hashValue(h, value);
  }
  hashValue(h, opacity);
  return h;
}

inline std::uint64_t pathTessellationScaleHash(float scaleX, float scaleY) noexcept {
  using namespace path_tessellation_cache_detail;
  std::uint64_t h = 14695981039346656037ULL;
  hashValue(h, scaleX);
  hashValue(h, scaleY);
  return h;
}

inline std::uint64_t pathTessellationStyleHash(FillStyle const& fill, StrokeStyle const& stroke,
                                               Mat3 const& transform, float opacity,
                                               std::uint64_t extraHash = 0) noexcept {
  return pathTessellationFillHash(fill) ^ (pathTessellationStrokeHash(stroke) + 0x9e3779b97f4a7c15ULL) ^
         (pathTessellationTransformHash(transform, opacity) << 1u) ^ (extraHash << 2u);
}

struct PathTessellationCacheKey {
  std::uint64_t pathHash = 0;
  std::uint64_t styleHash = 0;
  int viewportW = 0;
  int viewportH = 0;

  bool operator==(PathTessellationCacheKey const&) const = default;
};

struct PathTessellationCacheKeyHash {
  std::size_t operator()(PathTessellationCacheKey const& key) const noexcept {
    std::size_t h = static_cast<std::size_t>(key.pathHash);
    h ^= static_cast<std::size_t>(key.styleHash + 0x9e3779b97f4a7c15ULL + (h << 6u) + (h >> 2u));
    h ^= static_cast<std::size_t>(key.viewportW) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
    h ^= static_cast<std::size_t>(key.viewportH) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
    return h;
  }
};

template <typename Vertex>
class BasicPathTessellationCache {
public:
  using VertexList = std::vector<Vertex>;

  explicit BasicPathTessellationCache(std::size_t maxCachedVertices = 500'000)
      : maxCachedVertices_(maxCachedVertices) {}

  [[nodiscard]] VertexList const* find(PathTessellationCacheKey const& key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return nullptr;
    }
    lru_.splice(lru_.end(), lru_, it->second.lruIt);
    return &it->second.vertices;
  }

  bool insert(PathTessellationCacheKey key, VertexList vertices) {
    if (vertices.empty()) {
      return false;
    }
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
      cachedVertexCount_ -= existing->second.vertices.size();
      existing->second.vertices = std::move(vertices);
      cachedVertexCount_ += existing->second.vertices.size();
      lru_.splice(lru_.end(), lru_, existing->second.lruIt);
      trim();
      return false;
    }

    lru_.push_back(key);
    auto lruIt = std::prev(lru_.end());
    auto [it, inserted] = entries_.emplace(std::move(key), Entry{std::move(vertices), lruIt});
    if (inserted) {
      cachedVertexCount_ += it->second.vertices.size();
      trim();
    } else {
      lru_.erase(lruIt);
    }
    return inserted;
  }

  void trim() {
    while (cachedVertexCount_ > maxCachedVertices_ && !entries_.empty()) {
      if (lru_.empty()) {
        clear();
        return;
      }
      PathTessellationCacheKey const key = lru_.front();
      auto it = entries_.find(key);
      if (it != entries_.end()) {
        cachedVertexCount_ -= it->second.vertices.size();
        entries_.erase(it);
      }
      lru_.pop_front();
    }
  }

  void clear() {
    entries_.clear();
    lru_.clear();
    cachedVertexCount_ = 0;
  }

  [[nodiscard]] std::size_t entryCount() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t cachedVertexCount() const noexcept { return cachedVertexCount_; }
  [[nodiscard]] std::size_t maxCachedVertices() const noexcept { return maxCachedVertices_; }

private:
  struct Entry {
    VertexList vertices;
    typename std::list<PathTessellationCacheKey>::iterator lruIt;
  };

  std::size_t maxCachedVertices_ = 0;
  std::unordered_map<PathTessellationCacheKey, Entry, PathTessellationCacheKeyHash> entries_;
  std::list<PathTessellationCacheKey> lru_;
  std::size_t cachedVertexCount_ = 0;
};

using PathTessellationCache = BasicPathTessellationCache<PathVertex>;

} // namespace lambdaui

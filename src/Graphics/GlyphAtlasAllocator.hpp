#pragma once

#include <Lambda/Core/Geometry.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lambdaui {

struct GlyphAtlasKey {
  std::uint32_t fontId = 0;
  std::uint32_t glyphId = 0;
  std::uint16_t size = 0;

  bool operator==(GlyphAtlasKey const&) const = default;
};

struct GlyphAtlasKeyHash {
  std::size_t operator()(GlyphAtlasKey const& key) const noexcept;
};

struct GlyphAtlasEntry {
  std::uint16_t u = 0;
  std::uint16_t v = 0;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  Point bearing{};
  std::uint64_t lastUsed = 0;
};

struct GlyphAtlasPlacement {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
};

struct GlyphAtlasSize {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

class GlyphAtlasAllocator {
public:
  static constexpr std::uint32_t kDefaultMaxDimension = 4096;
  static constexpr std::uint32_t kDefaultPadding = 1;

  explicit GlyphAtlasAllocator(std::uint32_t width = 1024,
                               std::uint32_t height = 1024,
                               std::uint32_t maxDimension = kDefaultMaxDimension,
                               std::uint32_t padding = kDefaultPadding);

  std::uint32_t width() const noexcept { return width_; }
  std::uint32_t height() const noexcept { return height_; }
  std::uint32_t maxDimension() const noexcept { return maxDimension_; }
  std::uint32_t padding() const noexcept { return padding_; }
  std::size_t entryCount() const noexcept { return entries_.size(); }
  std::uint64_t useCounter() const noexcept { return useCounter_; }

  void reset(std::uint32_t width, std::uint32_t height);
  void resize(std::uint32_t width, std::uint32_t height);

  bool hasSpace(std::uint32_t glyphWidth, std::uint32_t glyphHeight) const;
  std::optional<GlyphAtlasPlacement> allocate(std::uint32_t glyphWidth,
                                              std::uint32_t glyphHeight);
  std::optional<GlyphAtlasSize> growthSizeFor(std::uint32_t glyphWidth,
                                              std::uint32_t glyphHeight) const;
  std::optional<GlyphAtlasSize> nextDoubledSize() const;
  bool pressureHighForHeadroom(std::uint32_t percent = 75) const;

  GlyphAtlasEntry const* find(GlyphAtlasKey const& key) const;
  GlyphAtlasEntry const& touch(GlyphAtlasKey const& key);
  std::uint64_t nextUseStamp();
  GlyphAtlasEntry const& insert(GlyphAtlasKey const& key,
                                GlyphAtlasPlacement placement,
                                std::uint32_t glyphWidth,
                                std::uint32_t glyphHeight,
                                Point bearing,
                                std::optional<std::uint64_t> lastUsed = std::nullopt);
  void erase(GlyphAtlasKey const& key);
  void clearEntries();

  std::vector<GlyphAtlasKey> oldestKeysToEvict(std::size_t divisor = 4) const;
  std::vector<std::pair<GlyphAtlasKey, GlyphAtlasEntry>> entriesNewestFirst() const;

private:
  bool canFitGlyph(std::uint32_t glyphWidth, std::uint32_t glyphHeight,
                   std::uint32_t width, std::uint32_t height) const;

  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t maxDimension_ = kDefaultMaxDimension;
  std::uint32_t padding_ = kDefaultPadding;
  std::uint32_t shelfX_ = kDefaultPadding;
  std::uint32_t shelfY_ = kDefaultPadding;
  std::uint32_t shelfH_ = 0;
  std::uint64_t useCounter_ = 0;
  std::unordered_map<GlyphAtlasKey, GlyphAtlasEntry, GlyphAtlasKeyHash> entries_;
};

} // namespace lambdaui

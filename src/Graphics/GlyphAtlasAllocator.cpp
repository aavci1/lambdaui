#include "Graphics/GlyphAtlasAllocator.hpp"

#include <algorithm>
#include <functional>

namespace lambdaui {

std::size_t GlyphAtlasKeyHash::operator()(GlyphAtlasKey const& key) const noexcept {
  std::size_t h = std::hash<std::uint32_t>{}(key.fontId);
  h ^= std::hash<std::uint32_t>{}(key.glyphId) + 0x9e3779b9 + (h << 6u) + (h >> 2u);
  h ^= std::hash<std::uint16_t>{}(key.size) + 0x9e3779b9 + (h << 6u) + (h >> 2u);
  return h;
}

GlyphAtlasAllocator::GlyphAtlasAllocator(std::uint32_t width,
                                         std::uint32_t height,
                                         std::uint32_t maxDimension,
                                         std::uint32_t padding)
    : width_(width)
    , height_(height)
    , maxDimension_(std::max(1u, maxDimension))
    , padding_(padding)
    , shelfX_(padding)
    , shelfY_(padding) {}

void GlyphAtlasAllocator::reset(std::uint32_t width, std::uint32_t height) {
  width_ = width;
  height_ = height;
  shelfX_ = padding_;
  shelfY_ = padding_;
  shelfH_ = 0;
  entries_.clear();
}

void GlyphAtlasAllocator::resize(std::uint32_t width, std::uint32_t height) {
  width_ = width;
  height_ = height;
}

bool GlyphAtlasAllocator::canFitGlyph(std::uint32_t glyphWidth,
                                      std::uint32_t glyphHeight,
                                      std::uint32_t width,
                                      std::uint32_t height) const {
  std::uint64_t const paddedWidth =
      static_cast<std::uint64_t>(glyphWidth) + static_cast<std::uint64_t>(padding_) * 2u;
  std::uint64_t const paddedHeight =
      static_cast<std::uint64_t>(glyphHeight) + static_cast<std::uint64_t>(padding_) * 2u;
  return paddedWidth < width && paddedHeight < height;
}

bool GlyphAtlasAllocator::hasSpace(std::uint32_t glyphWidth,
                                   std::uint32_t glyphHeight) const {
  if (!canFitGlyph(glyphWidth, glyphHeight, width_, height_)) {
    return false;
  }
  std::uint32_t x = shelfX_;
  std::uint32_t y = shelfY_;
  std::uint32_t rowH = shelfH_;
  std::uint32_t const cellW = glyphWidth + padding_ * 2u;
  std::uint32_t const cellH = glyphHeight + padding_ * 2u;
  if (x + cellW + padding_ > width_) {
    x = padding_;
    y += rowH + padding_;
    rowH = 0;
  }
  (void)rowH;
  return y + cellH + padding_ <= height_;
}

std::optional<GlyphAtlasPlacement> GlyphAtlasAllocator::allocate(std::uint32_t glyphWidth,
                                                                 std::uint32_t glyphHeight) {
  if (!hasSpace(glyphWidth, glyphHeight)) {
    return std::nullopt;
  }
  std::uint32_t const cellW = glyphWidth + padding_ * 2u;
  std::uint32_t const cellH = glyphHeight + padding_ * 2u;
  if (shelfX_ + cellW + padding_ > width_) {
    shelfY_ += shelfH_ + padding_;
    shelfX_ = padding_;
    shelfH_ = 0;
  }

  GlyphAtlasPlacement const placement{
      .x = shelfX_ + padding_,
      .y = shelfY_ + padding_,
  };
  shelfX_ += cellW + padding_;
  shelfH_ = std::max(shelfH_, cellH);
  return placement;
}

std::optional<GlyphAtlasSize> GlyphAtlasAllocator::growthSizeFor(std::uint32_t glyphWidth,
                                                                 std::uint32_t glyphHeight) const {
  if (hasSpace(glyphWidth, glyphHeight)) {
    return GlyphAtlasSize{width_, height_};
  }
  std::uint32_t nextWidth = width_;
  std::uint32_t nextHeight = height_;
  std::uint32_t const requiredWidth = glyphWidth + padding_ * 3u;
  std::uint32_t const requiredHeight = glyphHeight + padding_ * 3u;
  while (nextWidth < maxDimension_ || nextHeight < maxDimension_) {
    std::uint32_t const doubledWidth = std::max(nextWidth, padding_ * 4u) * 2u;
    std::uint32_t const doubledHeight = std::max(nextHeight, padding_ * 4u) * 2u;
    nextWidth = std::min(maxDimension_, std::max(doubledWidth, requiredWidth));
    nextHeight = std::min(maxDimension_, std::max(doubledHeight, requiredHeight));
    if (canFitGlyph(glyphWidth, glyphHeight, nextWidth, nextHeight)) {
      return GlyphAtlasSize{nextWidth, nextHeight};
    }
  }
  return std::nullopt;
}

std::optional<GlyphAtlasSize> GlyphAtlasAllocator::nextDoubledSize() const {
  if (width_ >= maxDimension_ && height_ >= maxDimension_) {
    return std::nullopt;
  }
  return GlyphAtlasSize{
      .width = std::min(width_ * 2u, maxDimension_),
      .height = std::min(height_ * 2u, maxDimension_),
  };
}

bool GlyphAtlasAllocator::pressureHighForHeadroom(std::uint32_t percent) const {
  if (width_ >= maxDimension_ || height_ >= maxDimension_) {
    return false;
  }
  std::uint64_t const usedBottom = static_cast<std::uint64_t>(shelfY_) +
                                   static_cast<std::uint64_t>(shelfH_);
  return usedBottom * 100u > static_cast<std::uint64_t>(height_) * percent;
}

GlyphAtlasEntry const* GlyphAtlasAllocator::find(GlyphAtlasKey const& key) const {
  auto it = entries_.find(key);
  return it == entries_.end() ? nullptr : &it->second;
}

GlyphAtlasEntry const& GlyphAtlasAllocator::touch(GlyphAtlasKey const& key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    static GlyphAtlasEntry const empty{};
    return empty;
  }
  it->second.lastUsed = nextUseStamp();
  return it->second;
}

std::uint64_t GlyphAtlasAllocator::nextUseStamp() {
  return ++useCounter_;
}

GlyphAtlasEntry const& GlyphAtlasAllocator::insert(GlyphAtlasKey const& key,
                                                   GlyphAtlasPlacement placement,
                                                   std::uint32_t glyphWidth,
                                                   std::uint32_t glyphHeight,
                                                   Point bearing,
                                                   std::optional<std::uint64_t> lastUsed) {
  GlyphAtlasEntry entry{};
  entry.u = static_cast<std::uint16_t>(placement.x);
  entry.v = static_cast<std::uint16_t>(placement.y);
  entry.width = static_cast<std::uint16_t>(glyphWidth);
  entry.height = static_cast<std::uint16_t>(glyphHeight);
  entry.bearing = bearing;
  entry.lastUsed = lastUsed.value_or(nextUseStamp());
  useCounter_ = std::max(useCounter_, entry.lastUsed);
  auto [it, inserted] = entries_.insert_or_assign(key, entry);
  (void)inserted;
  return it->second;
}

void GlyphAtlasAllocator::erase(GlyphAtlasKey const& key) {
  entries_.erase(key);
}

void GlyphAtlasAllocator::clearEntries() {
  entries_.clear();
}

std::vector<GlyphAtlasKey> GlyphAtlasAllocator::oldestKeysToEvict(std::size_t divisor) const {
  std::vector<std::pair<GlyphAtlasKey, std::uint64_t>> byAge;
  byAge.reserve(entries_.size());
  for (auto const& [key, entry] : entries_) {
    if (entry.width > 0 && entry.height > 0) {
      byAge.push_back({key, entry.lastUsed});
    }
  }
  std::sort(byAge.begin(), byAge.end(), [](auto const& a, auto const& b) {
    return a.second < b.second;
  });
  std::size_t const eraseCount = divisor == 0
                                     ? byAge.size()
                                     : std::max<std::size_t>(1, byAge.size() / divisor);
  std::vector<GlyphAtlasKey> keys;
  keys.reserve(std::min(eraseCount, byAge.size()));
  for (std::size_t i = 0; i < eraseCount && i < byAge.size(); ++i) {
    keys.push_back(byAge[i].first);
  }
  return keys;
}

std::vector<std::pair<GlyphAtlasKey, GlyphAtlasEntry>>
GlyphAtlasAllocator::entriesNewestFirst() const {
  std::vector<std::pair<GlyphAtlasKey, GlyphAtlasEntry>> entries;
  entries.reserve(entries_.size());
  for (auto const& entry : entries_) {
    if (entry.second.width > 0 && entry.second.height > 0) {
      entries.push_back(entry);
    }
  }
  std::sort(entries.begin(), entries.end(), [](auto const& a, auto const& b) {
    return a.second.lastUsed > b.second.lastUsed;
  });
  return entries;
}

} // namespace lambdaui

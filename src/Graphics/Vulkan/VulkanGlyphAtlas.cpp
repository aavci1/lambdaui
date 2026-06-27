#include "Graphics/Vulkan/VulkanGlyphAtlas.hpp"

#include <Lambda/Debug/PerfCounters.hpp>
#include <Lambda/Graphics/TextSystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace lambdaui {

namespace {

constexpr int kVulkanGlyphAtlasMaxDim = 4096;
constexpr int kAtlasPad = 1;

} // namespace

VulkanGlyphAtlas::VulkanGlyphAtlas(TextSystem& textSystem)
    : textSystem_(textSystem) {}

bool VulkanGlyphAtlas::atlasHasSpace(SharedVulkanCore::Resources const& resources,
                                     std::uint32_t width,
                                     std::uint32_t height) const {
  if (resources.atlas.width <= kAtlasPad * 2 || resources.atlas.height <= kAtlasPad * 2) {
    return false;
  }
  if (width + static_cast<std::uint32_t>(kAtlasPad * 2) >= static_cast<std::uint32_t>(resources.atlas.width) ||
      height + static_cast<std::uint32_t>(kAtlasPad * 2) >= static_cast<std::uint32_t>(resources.atlas.height)) {
    return false;
  }
  int x = resources.atlasX;
  int y = resources.atlasY;
  int rowH = resources.atlasRowH;
  if (x + static_cast<int>(width) + kAtlasPad >= resources.atlas.width) {
    x = kAtlasPad;
    y += rowH + kAtlasPad;
  }
  return y + static_cast<int>(height) + kAtlasPad < resources.atlas.height;
}

void VulkanGlyphAtlas::updateSlotTexCoords(SharedVulkanCore::Resources const& resources,
                                           VulkanGlyphSlot& slot) const {
  if (resources.atlas.width <= 0 || resources.atlas.height <= 0) {
    slot.u0 = 0.f;
    slot.v0 = 0.f;
    slot.u1 = 0.f;
    slot.v1 = 0.f;
    return;
  }
  slot.u0 = static_cast<float>(slot.x) / resources.atlas.width;
  slot.v0 = static_cast<float>(slot.y) / resources.atlas.height;
  slot.u1 = static_cast<float>(slot.x + static_cast<int>(slot.w)) / resources.atlas.width;
  slot.v1 = static_cast<float>(slot.y + static_cast<int>(slot.h)) / resources.atlas.height;
}

bool VulkanGlyphAtlas::growAtlasForSpace(SharedVulkanCore::Resources& resources,
                                         std::uint32_t width,
                                         std::uint32_t height) {
  bool grew = false;
  while (!atlasHasSpace(resources, width, height) &&
         (resources.atlas.width < kVulkanGlyphAtlasMaxDim ||
          resources.atlas.height < kVulkanGlyphAtlasMaxDim)) {
    int const oldWidth = resources.atlas.width;
    int const oldHeight = resources.atlas.height;
    int const requiredWidth = static_cast<int>(width) + kAtlasPad * 3;
    int const requiredHeight = static_cast<int>(height) + kAtlasPad * 3;
    int const doubledWidth = std::max(oldWidth, kAtlasPad * 4) * 2;
    int const doubledHeight = std::max(oldHeight, kAtlasPad * 4) * 2;
    int const newWidth = std::min(kVulkanGlyphAtlasMaxDim, std::max(doubledWidth, requiredWidth));
    int const newHeight = std::min(kVulkanGlyphAtlasMaxDim, std::max(doubledHeight, requiredHeight));
    if (newWidth == oldWidth && newHeight == oldHeight) {
      break;
    }

    std::vector<Rgba> newPixels(static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight),
                                Rgba{255, 255, 255, 0});
    if (oldWidth > 0 && oldHeight > 0 &&
        resources.atlasPixels.size() >= static_cast<std::size_t>(oldWidth) * static_cast<std::size_t>(oldHeight)) {
      for (int row = 0; row < oldHeight; ++row) {
        auto const oldBegin = resources.atlasPixels.begin() +
            static_cast<std::ptrdiff_t>(row) * oldWidth;
        auto const oldEnd = oldBegin + oldWidth;
        auto newBegin = newPixels.begin() + static_cast<std::ptrdiff_t>(row) * newWidth;
        std::copy(oldBegin, oldEnd, newBegin);
      }
    }

    resources.atlasPixels = std::move(newPixels);
    resources.atlas.width = newWidth;
    resources.atlas.height = newHeight;
    for (auto& [key, slot] : resources.glyphs) {
      (void)key;
      updateSlotTexCoords(resources, slot);
    }
    ++resources.atlasGeneration;
    resources.atlasDirty = true;
    debug::perf::recordGlyphAtlasGrowth(static_cast<std::uint64_t>(std::max(oldWidth, 0)) *
                                            static_cast<std::uint64_t>(std::max(oldHeight, 0)),
                                        static_cast<std::uint64_t>(newWidth) *
                                            static_cast<std::uint64_t>(newHeight));
    grew = true;
  }
  return grew && atlasHasSpace(resources, width, height);
}

void VulkanGlyphAtlas::placeGlyphInAtlas(SharedVulkanCore::Resources& resources, VulkanGlyphSlot& slot) {
  if (resources.atlasX + static_cast<int>(slot.w) + kAtlasPad >= resources.atlas.width) {
    resources.atlasX = kAtlasPad;
    resources.atlasY += resources.atlasRowH + kAtlasPad;
    resources.atlasRowH = 0;
  }
  slot.x = resources.atlasX;
  slot.y = resources.atlasY;
  resources.atlasX += static_cast<int>(slot.w) + kAtlasPad;
  resources.atlasRowH = std::max(resources.atlasRowH, static_cast<int>(slot.h));
  for (std::uint32_t row = 0; row < slot.h; ++row) {
    for (std::uint32_t col = 0; col < slot.w; ++col) {
      Rgba& px =
          resources.atlasPixels[static_cast<std::size_t>(slot.y + row) * resources.atlas.width + slot.x + col];
      px = {255, 255, 255, slot.alpha[static_cast<std::size_t>(row) * slot.w + col]};
    }
  }
  updateSlotTexCoords(resources, slot);
}

void VulkanGlyphAtlas::rebuildAtlas(SharedVulkanCore::Resources& resources) {
  std::vector<std::pair<VulkanGlyphKey, VulkanGlyphSlot>> slots;
  slots.reserve(resources.glyphs.size());
  for (auto const& [key, slot] : resources.glyphs) {
    if (slot.w > 0 && slot.h > 0 && !slot.alpha.empty()) {
      slots.push_back({key, slot});
    }
  }
  std::sort(slots.begin(), slots.end(), [](auto const& a, auto const& b) {
    return a.second.lastUsed > b.second.lastUsed;
  });
  resources.atlasPixels.assign(static_cast<std::size_t>(resources.atlas.width) * resources.atlas.height,
                               Rgba{255, 255, 255, 0});
  resources.atlasX = 1;
  resources.atlasY = 1;
  resources.atlasRowH = 0;
  resources.glyphs.clear();
  ++resources.atlasGeneration;
  for (auto& [key, slot] : slots) {
    if (!atlasHasSpace(resources, slot.w, slot.h)) {
      continue;
    }
    placeGlyphInAtlas(resources, slot);
    resources.glyphs.emplace(key, std::move(slot));
  }
  resources.atlasDirty = true;
}

bool VulkanGlyphAtlas::evictGlyphsForAtlasSpace(SharedVulkanCore::Resources& resources,
                                                std::uint32_t width,
                                                std::uint32_t height) {
  if (growAtlasForSpace(resources, width, height)) {
    return true;
  }
  if (width + 2u >= static_cast<std::uint32_t>(resources.atlas.width) ||
      height + 2u >= static_cast<std::uint32_t>(resources.atlas.height)) {
    return false;
  }
  if (atlasHasSpace(resources, width, height)) {
    return true;
  }
  if (resources.glyphs.empty()) {
    return false;
  }

  std::vector<std::pair<VulkanGlyphKey, std::uint64_t>> byAge;
  byAge.reserve(resources.glyphs.size());
  for (auto const& [key, slot] : resources.glyphs) {
    if (slot.w > 0 && slot.h > 0) {
      byAge.push_back({key, slot.lastUsed});
    }
  }
  if (byAge.empty()) {
    return false;
  }
  std::sort(byAge.begin(), byAge.end(), [](auto const& a, auto const& b) {
    return a.second < b.second;
  });
  std::size_t const eraseCount = std::max<std::size_t>(1, byAge.size() / 4);
  for (std::size_t i = 0; i < eraseCount && i < byAge.size(); ++i) {
    resources.glyphs.erase(byAge[i].first);
  }
  if (!byAge.empty()) {
    rebuildAtlas(resources);
  }
  return atlasHasSpace(resources, width, height);
}

VulkanGlyphSlot const* VulkanGlyphAtlas::glyphSlot(SharedVulkanCore::Resources& resources,
                                                   float dpiScaleY,
                                                   std::uint32_t fontId,
                                                   std::uint32_t glyphId,
                                                   float fontSize) {
  std::uint16_t size =
      static_cast<std::uint16_t>(std::clamp(std::round(fontSize * dpiScaleY), 1.f, 512.f));
  VulkanGlyphKey key{fontId, glyphId, size};
  auto it = resources.glyphs.find(key);
  if (it != resources.glyphs.end()) {
    it->second.lastUsed = ++resources.atlasUseCounter;
    return &it->second;
  }
  std::uint32_t gw = 0;
  std::uint32_t gh = 0;
  Point bearing{};
  std::vector<std::uint8_t> alpha =
      textSystem_.rasterizeGlyph(fontId, glyphId, static_cast<float>(size), gw, gh, bearing);
  if (gw == 0 || gh == 0 || alpha.empty()) {
    VulkanGlyphSlot empty{};
    empty.lastUsed = ++resources.atlasUseCounter;
    auto [inserted, ok] = resources.glyphs.emplace(key, std::move(empty));
    (void)ok;
    return &inserted->second;
  }
  if (!atlasHasSpace(resources, gw, gh) && !evictGlyphsForAtlasSpace(resources, gw, gh)) {
    return nullptr;
  }
  VulkanGlyphSlot slot{};
  slot.w = gw;
  slot.h = gh;
  slot.bearing = bearing;
  slot.lastUsed = ++resources.atlasUseCounter;
  slot.alpha = std::move(alpha);
  placeGlyphInAtlas(resources, slot);
  resources.atlasDirty = true;
  auto [inserted, ok] = resources.glyphs.emplace(key, slot);
  (void)ok;
  return &inserted->second;
}

bool VulkanGlyphAtlas::atlasUploadNeeded(SharedVulkanCore::Resources const& resources) const noexcept {
  return resources.atlasDirty;
}

} // namespace lambdaui

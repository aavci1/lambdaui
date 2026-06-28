#include "Graphics/Vulkan/VulkanGlyphAtlas.hpp"

#include <Lambda/Debug/PerfCounters.hpp>
#include <Lambda/Graphics/TextSystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace lambdaui {

VulkanGlyphAtlas::VulkanGlyphAtlas(TextSystem& textSystem)
    : textSystem_(textSystem) {}

void VulkanGlyphAtlas::syncAllocatorToAtlas(SharedVulkanCore::Resources& resources) const {
  if (resources.atlas.width <= 0 || resources.atlas.height <= 0) {
    return;
  }
  auto& allocator = resources.glyphAtlasAllocator;
  std::uint32_t const width = static_cast<std::uint32_t>(resources.atlas.width);
  std::uint32_t const height = static_cast<std::uint32_t>(resources.atlas.height);
  if (allocator.width() == width && allocator.height() == height) {
    return;
  }
  if (resources.glyphs.empty() && allocator.entryCount() == 0) {
    allocator.reset(width, height);
  } else {
    allocator.resize(width, height);
  }
}

bool VulkanGlyphAtlas::atlasHasSpace(SharedVulkanCore::Resources& resources,
                                     std::uint32_t width,
                                     std::uint32_t height) const {
  syncAllocatorToAtlas(resources);
  return resources.glyphAtlasAllocator.hasSpace(width, height);
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
  syncAllocatorToAtlas(resources);
  bool grew = false;
  while (!resources.glyphAtlasAllocator.hasSpace(width, height)) {
    std::optional<GlyphAtlasSize> const nextSize =
        resources.glyphAtlasAllocator.growthSizeFor(width, height);
    if (!nextSize) {
      break;
    }

    int const oldWidth = resources.atlas.width;
    int const oldHeight = resources.atlas.height;
    int const newWidth = static_cast<int>(nextSize->width);
    int const newHeight = static_cast<int>(nextSize->height);
    if (newWidth == oldWidth && newHeight == oldHeight) {
      break;
    }

    std::vector<Rgba> newPixels(static_cast<std::size_t>(newWidth) *
                                    static_cast<std::size_t>(newHeight),
                                Rgba{255, 255, 255, 0});
    if (oldWidth > 0 && oldHeight > 0 &&
        resources.atlasPixels.size() >=
            static_cast<std::size_t>(oldWidth) * static_cast<std::size_t>(oldHeight)) {
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
    resources.glyphAtlasAllocator.resize(nextSize->width, nextSize->height);
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
  return grew && resources.glyphAtlasAllocator.hasSpace(width, height);
}

bool VulkanGlyphAtlas::placeGlyphInAtlas(SharedVulkanCore::Resources& resources,
                                         VulkanGlyphKey const& key,
                                         VulkanGlyphSlot& slot,
                                         std::optional<std::uint64_t> lastUsed) {
  syncAllocatorToAtlas(resources);
  std::optional<GlyphAtlasPlacement> const placement =
      resources.glyphAtlasAllocator.allocate(slot.w, slot.h);
  if (!placement) {
    return false;
  }

  slot.x = static_cast<int>(placement->x);
  slot.y = static_cast<int>(placement->y);
  GlyphAtlasEntry const& entry = resources.glyphAtlasAllocator.insert(
      key, *placement, slot.w, slot.h, slot.bearing, lastUsed);
  slot.lastUsed = entry.lastUsed;
  resources.atlasUseCounter = resources.glyphAtlasAllocator.useCounter();

  for (std::uint32_t row = 0; row < slot.h; ++row) {
    for (std::uint32_t col = 0; col < slot.w; ++col) {
      Rgba& px =
          resources.atlasPixels[static_cast<std::size_t>(slot.y + row) * resources.atlas.width + slot.x + col];
      px = {255, 255, 255, slot.alpha[static_cast<std::size_t>(row) * slot.w + col]};
    }
  }
  updateSlotTexCoords(resources, slot);
  return true;
}

void VulkanGlyphAtlas::rebuildAtlas(SharedVulkanCore::Resources& resources) {
  syncAllocatorToAtlas(resources);
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

  resources.atlasPixels.assign(static_cast<std::size_t>(resources.atlas.width) *
                                   static_cast<std::size_t>(resources.atlas.height),
                               Rgba{255, 255, 255, 0});
  resources.glyphAtlasAllocator.reset(static_cast<std::uint32_t>(resources.atlas.width),
                                      static_cast<std::uint32_t>(resources.atlas.height));
  resources.glyphs.clear();
  ++resources.atlasGeneration;
  for (auto& [key, slot] : slots) {
    if (!atlasHasSpace(resources, slot.w, slot.h)) {
      continue;
    }
    if (placeGlyphInAtlas(resources, key, slot, slot.lastUsed)) {
      resources.glyphs.emplace(key, std::move(slot));
    }
  }
  resources.atlasDirty = true;
}

bool VulkanGlyphAtlas::evictGlyphsForAtlasSpace(SharedVulkanCore::Resources& resources,
                                                std::uint32_t width,
                                                std::uint32_t height) {
  if (growAtlasForSpace(resources, width, height)) {
    return true;
  }
  if (atlasHasSpace(resources, width, height)) {
    return true;
  }
  if (resources.glyphAtlasAllocator.entryCount() == 0) {
    return false;
  }

  std::vector<GlyphAtlasKey> const keys = resources.glyphAtlasAllocator.oldestKeysToEvict();
  if (keys.empty()) {
    return false;
  }
  for (GlyphAtlasKey const& key : keys) {
    resources.glyphs.erase(key);
    resources.glyphAtlasAllocator.erase(key);
  }
  rebuildAtlas(resources);
  return atlasHasSpace(resources, width, height);
}

VulkanGlyphSlot const* VulkanGlyphAtlas::glyphSlot(SharedVulkanCore::Resources& resources,
                                                   float dpiScaleY,
                                                   std::uint32_t fontId,
                                                   std::uint32_t glyphId,
                                                   float fontSize) {
  syncAllocatorToAtlas(resources);
  std::uint16_t size =
      static_cast<std::uint16_t>(std::clamp(std::round(fontSize * dpiScaleY), 1.f, 512.f));
  VulkanGlyphKey key{fontId, glyphId, size};
  auto it = resources.glyphs.find(key);
  if (it != resources.glyphs.end()) {
    if (resources.glyphAtlasAllocator.find(key)) {
      it->second.lastUsed = resources.glyphAtlasAllocator.touch(key).lastUsed;
    } else {
      it->second.lastUsed = resources.glyphAtlasAllocator.nextUseStamp();
    }
    resources.atlasUseCounter = resources.glyphAtlasAllocator.useCounter();
    return &it->second;
  }

  std::uint32_t gw = 0;
  std::uint32_t gh = 0;
  Point bearing{};
  std::vector<std::uint8_t> alpha =
      textSystem_.rasterizeGlyph(fontId, glyphId, static_cast<float>(size), gw, gh, bearing);
  if (gw == 0 || gh == 0 || alpha.empty()) {
    VulkanGlyphSlot empty{};
    empty.lastUsed = resources.glyphAtlasAllocator.nextUseStamp();
    resources.atlasUseCounter = resources.glyphAtlasAllocator.useCounter();
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
  slot.alpha = std::move(alpha);
  if (!placeGlyphInAtlas(resources, key, slot)) {
    return nullptr;
  }
  resources.atlasDirty = true;
  auto [inserted, ok] = resources.glyphs.emplace(key, slot);
  (void)ok;
  return &inserted->second;
}

bool VulkanGlyphAtlas::atlasUploadNeeded(SharedVulkanCore::Resources const& resources) const noexcept {
  return resources.atlasDirty;
}

} // namespace lambdaui

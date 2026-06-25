#pragma once

#include "Graphics/Vulkan/VulkanCanvasShared.hpp"

#include <cstdint>

namespace lambdaui {

class TextSystem;

class VulkanGlyphAtlas {
public:
  explicit VulkanGlyphAtlas(TextSystem& textSystem);

  VulkanGlyphSlot const* glyphSlot(SharedVulkanCore::Resources& resources,
                                   float dpiScaleY,
                                   std::uint32_t fontId,
                                   std::uint32_t glyphId,
                                   float fontSize);

  bool atlasUploadNeeded(SharedVulkanCore::Resources const& resources) const noexcept;

private:
  bool atlasHasSpace(SharedVulkanCore::Resources const& resources,
                     std::uint32_t width,
                     std::uint32_t height) const;
  void placeGlyphInAtlas(SharedVulkanCore::Resources& resources, VulkanGlyphSlot& slot);
  void rebuildAtlas(SharedVulkanCore::Resources& resources);
  bool evictGlyphsForAtlasSpace(SharedVulkanCore::Resources& resources,
                                std::uint32_t width,
                                std::uint32_t height);

  TextSystem& textSystem_;
};

} // namespace lambdaui

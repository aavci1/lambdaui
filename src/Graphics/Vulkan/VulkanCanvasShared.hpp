#pragma once

#include "Graphics/GlyphAtlasAllocator.hpp"
#include "Graphics/Vulkan/VulkanCanvasTypes.hpp"

#include <Lambda/Core/Geometry.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;

namespace lambdaui {

inline constexpr std::size_t kVulkanMaxFramesInFlight = 3;
inline constexpr std::size_t kVulkanBlendModePipelineCount = static_cast<std::size_t>(BlendMode::Xor) + 1u;

inline std::size_t vulkanBlendModeIndex(BlendMode mode) noexcept {
  std::size_t const index = static_cast<std::size_t>(mode);
  return index < kVulkanBlendModePipelineCount ? index : 0u;
}

struct Rgba {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;
};

using VulkanGlyphKey = GlyphAtlasKey;
using VulkanGlyphKeyHash = GlyphAtlasKeyHash;

struct VulkanGlyphSlot {
  float u0 = 0;
  float v0 = 0;
  float u1 = 0;
  float v1 = 0;
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  int x = 0;
  int y = 0;
  Point bearing{};
  std::uint64_t lastUsed = 0;
  std::vector<std::uint8_t> alpha;
};

struct VulkanDrawPushConstants {
  float viewport[2]{};
  float translation[2]{};
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkDeviceSize capacity = 0;
  void* mapped = nullptr;
};

struct FrameGeometryResources {
  VkDescriptorSet rectDescriptorSet = VK_NULL_HANDLE;
  VkDescriptorSet calloutDescriptorSet = VK_NULL_HANDLE;
  VkDescriptorSet quadDescriptorSet = VK_NULL_HANDLE;
  Buffer rectBuffer;
  Buffer calloutBuffer;
  Buffer quadBuffer;
  Buffer pathBuffer;
};

struct SharedVulkanCore {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VmaAllocator allocator = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t queueFamily = 0;

  struct Resources {
    bool initialized = false;
    VkFormat renderFormat = VK_FORMAT_UNDEFINED;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout rectDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout quadDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureDescriptorLayout = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipelineLayout rectPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout calloutPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout imagePipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout pathPipelineLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, kVulkanBlendModePipelineCount> rectPipelines{};
    std::array<VkPipeline, kVulkanBlendModePipelineCount> calloutPipelines{};
    std::array<VkPipeline, kVulkanBlendModePipelineCount> imagePipelines{};
    std::array<VkPipeline, kVulkanBlendModePipelineCount> imageUnpremultiplyPipelines{};
    std::array<VkPipeline, kVulkanBlendModePipelineCount> pathPipelines{};
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::filesystem::path pipelineCacheFile;
    Texture atlas;
    std::vector<Rgba> atlasPixels;
    int atlasTextureWidth = 0;
    int atlasTextureHeight = 0;
    bool atlasDirty = false;
    std::uint64_t atlasGeneration = 1;
    std::uint64_t atlasUseCounter = 0;
    GlyphAtlasAllocator glyphAtlasAllocator;
    std::unordered_map<VulkanGlyphKey, VulkanGlyphSlot, VulkanGlyphKeyHash> glyphs;
  } resources;

  std::uint32_t refs = 0;
  bool googleDisplayTiming = false;
  bool swapchainMaintenance1 = false;
  std::string swapchainMaintenance1Extension;
  VkDriverId driverId = VK_DRIVER_ID_MAX_ENUM;
  std::string driverName;
  std::string driverInfo;
};

struct RetiredSwapchain {
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  std::vector<VkImageView> views;
  std::vector<VkSemaphore> renderFinished;
  std::vector<VkFence> presentFences;
  std::array<std::uint64_t, kVulkanMaxFramesInFlight> frameFenceGenerations{};
};

} // namespace lambdaui

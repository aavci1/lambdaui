#pragma once

#include <Lambda/Graphics/Image.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <vector>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;
struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;

namespace lambdaui {

struct PendingOwnedVulkanImageDestroy {
  VkDevice device = VK_NULL_HANDLE;
  VmaAllocator allocator = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory importedMemory = VK_NULL_HANDLE;
  bool ownsImportedMemory = false;
  std::uint32_t framesRemaining = 0;
  std::uint32_t successfulSubmits = 0;
};

struct VulkanImage final : Image {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;
  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  mutable VkImage image = VK_NULL_HANDLE;
  mutable VmaAllocation allocation = VK_NULL_HANDLE;
  VkDeviceMemory importedMemory = VK_NULL_HANDLE;
  mutable VkImageView view = VK_NULL_HANDLE;
  mutable VkDescriptorSet descriptor = VK_NULL_HANDLE;
  mutable bool uploaded = false;
  bool external = false;
  bool ownsGpuResource = false;
  bool ownsImportedMemory = false;
  bool ownsCoreReference = false;
  VkDevice owningDevice = VK_NULL_HANDLE;
  VmaAllocator owningAllocator = VK_NULL_HANDLE;
  std::uint64_t contentGeneration = 1;

  VulkanImage(int w, int h, std::vector<std::uint8_t> p, VkFormat pixelFormat);
  VulkanImage(VkImage externalImage,
              VkImageView externalView,
              VkFormat externalFormat,
              std::uint32_t w,
              std::uint32_t h);
  VulkanImage(VkDevice device,
              VmaAllocator allocator,
              VkImage ownedImage,
              VmaAllocation ownedAllocation,
              VkImageView ownedView,
              VkFormat imageFormat,
              int w,
              int h);
  VulkanImage(VkDevice device,
              VkImage importedImage,
              VkDeviceMemory importedImageMemory,
              VkImageView importedView,
              VkFormat imageFormat,
              int w,
              int h);
  ~VulkanImage() override;

  Size size() const override;
  void markContentChanged();
  bool updateRgbaPixels(std::span<std::uint8_t const> rgbaPixels, void* gpuDevice) override;
  bool updatePixels(std::span<std::uint8_t const> newPixels,
                    PixelFormat pixelFormat,
                    void* gpuDevice) override;
  bool updatePixelsRegion(std::span<std::uint8_t const> newPixels,
                          PixelFormat pixelFormat,
                          std::uint32_t x,
                          std::uint32_t y,
                          std::uint32_t regionWidth,
                          std::uint32_t regionHeight,
                          void* gpuDevice,
                          std::uint32_t sourceBytesPerRow) override;
};

VkFormat vkFormatForImagePixelFormat(Image::PixelFormat format);
void destroyOwnedVulkanImage(PendingOwnedVulkanImageDestroy& destroy);

} // namespace lambdaui

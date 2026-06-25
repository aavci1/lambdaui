#include "Graphics/Vulkan/VulkanImage.hpp"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "vma/vk_mem_alloc.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstring>
#include <limits>
#include <utility>

namespace lambdaui {

void evictVulkanImageTextures(VulkanImage const* image) noexcept;
void updateVulkanImageTextures(VulkanImage const* image);
void updateVulkanImageTextureRegion(VulkanImage const* image,
                                    std::uint32_t x,
                                    std::uint32_t y,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    void const* pixels,
                                    std::uint32_t sourceBytesPerRow);
void markVulkanImageTextureContentsChanged(VulkanImage const* image);
bool deferVulkanImageResourceDestroy(PendingOwnedVulkanImageDestroy destroy);
void retainSharedVulkanCoreForVulkanImage();
void releaseSharedVulkanCoreForVulkanImage();

VkFormat vkFormatForImagePixelFormat(Image::PixelFormat format) {
  switch (format) {
  case Image::PixelFormat::Bgra8888:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case Image::PixelFormat::Rgba8888:
  default:
    return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

void destroyOwnedVulkanImage(PendingOwnedVulkanImageDestroy& destroy) {
  if (destroy.view && destroy.device) {
    vkDestroyImageView(destroy.device, destroy.view, nullptr);
  }
  if (destroy.image && destroy.allocator) {
    vmaDestroyImage(destroy.allocator, destroy.image, destroy.allocation);
  } else if (destroy.image && destroy.device) {
    vkDestroyImage(destroy.device, destroy.image, nullptr);
  }
  if (destroy.ownsImportedMemory && destroy.importedMemory && destroy.device) {
    vkFreeMemory(destroy.device, destroy.importedMemory, nullptr);
  }
  destroy = {};
}

VulkanImage::VulkanImage(int w, int h, std::vector<std::uint8_t> p, VkFormat pixelFormat)
    : width(w),
      height(h),
      pixels(std::move(p)),
      format(pixelFormat == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8G8B8A8_UNORM : pixelFormat) {}

VulkanImage::VulkanImage(VkImage externalImage,
                         VkImageView externalView,
                         VkFormat externalFormat,
                         std::uint32_t w,
                         std::uint32_t h)
    : width(static_cast<int>(w)),
      height(static_cast<int>(h)),
      format(externalFormat == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8G8B8A8_UNORM : externalFormat),
      image(externalImage),
      view(externalView),
      uploaded(true),
      external(true) {}

VulkanImage::VulkanImage(VkDevice device,
                         VmaAllocator allocator,
                         VkImage ownedImage,
                         VmaAllocation ownedAllocation,
                         VkImageView ownedView,
                         VkFormat imageFormat,
                         int w,
                         int h,
                         bool imagePremultipliedAlpha)
    : width(w),
      height(h),
      format(imageFormat == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : imageFormat),
      image(ownedImage),
      allocation(ownedAllocation),
      view(ownedView),
      uploaded(true),
      ownsGpuResource(true),
      premultipliedAlpha_(imagePremultipliedAlpha),
      owningDevice(device),
      owningAllocator(allocator) {
  retainSharedVulkanCoreForVulkanImage();
  ownsCoreReference = true;
}

VulkanImage::VulkanImage(VkDevice device,
                         VkImage importedImage,
                         VkDeviceMemory importedImageMemory,
                         VkImageView importedView,
                         VkFormat imageFormat,
                         int w,
                         int h)
    : width(w),
      height(h),
      format(imageFormat == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : imageFormat),
      image(importedImage),
      importedMemory(importedImageMemory),
      view(importedView),
      uploaded(true),
      ownsGpuResource(true),
      ownsImportedMemory(true),
      owningDevice(device) {
  retainSharedVulkanCoreForVulkanImage();
  ownsCoreReference = true;
}

VulkanImage::~VulkanImage() {
  evictVulkanImageTextures(this);
  if (ownsGpuResource) {
    PendingOwnedVulkanImageDestroy pending{
        .device = owningDevice,
        .allocator = owningAllocator,
        .image = image,
        .allocation = allocation,
        .view = view,
        .importedMemory = importedMemory,
        .ownsImportedMemory = ownsImportedMemory,
    };
    if (!deferVulkanImageResourceDestroy(pending) && owningDevice) {
      vkDeviceWaitIdle(owningDevice);
      destroyOwnedVulkanImage(pending);
    }
    image = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
    importedMemory = VK_NULL_HANDLE;
  }
  if (ownsCoreReference) {
    releaseSharedVulkanCoreForVulkanImage();
  }
}

Size VulkanImage::size() const {
  return {static_cast<float>(width), static_cast<float>(height)};
}

void VulkanImage::markContentChanged() {
  contentGeneration =
      contentGeneration == std::numeric_limits<std::uint64_t>::max() ? 1 : contentGeneration + 1;
  markVulkanImageTextureContentsChanged(this);
}

bool VulkanImage::updateRgbaPixels(std::span<std::uint8_t const> rgbaPixels, void*) {
  return updatePixels(rgbaPixels, PixelFormat::Rgba8888, nullptr);
}

bool VulkanImage::updatePixels(std::span<std::uint8_t const> newPixels,
                               PixelFormat pixelFormat,
                               void*) {
  if (external || ownsGpuResource || width <= 0 || height <= 0) {
    return false;
  }
  if (format != vkFormatForImagePixelFormat(pixelFormat)) {
    return false;
  }
  std::size_t const expectedSize = static_cast<std::size_t>(width) * height * 4u;
  if (newPixels.size() != expectedSize) {
    return false;
  }
  if (pixels.size() != expectedSize) {
    pixels.resize(expectedSize);
  }
  std::memcpy(pixels.data(), newPixels.data(), expectedSize);
  markContentChanged();
  updateVulkanImageTextures(this);
  return true;
}

bool VulkanImage::updatePixelsRegion(std::span<std::uint8_t const> newPixels,
                                     PixelFormat pixelFormat,
                                     std::uint32_t x,
                                     std::uint32_t y,
                                     std::uint32_t regionWidth,
                                     std::uint32_t regionHeight,
                                     void*,
                                     std::uint32_t sourceBytesPerRow) {
  if (external || ownsGpuResource || width <= 0 || height <= 0 || regionWidth == 0 ||
      regionHeight == 0) {
    return false;
  }
  if (format != vkFormatForImagePixelFormat(pixelFormat)) {
    return false;
  }
  if (x > static_cast<std::uint32_t>(width) || y > static_cast<std::uint32_t>(height) ||
      regionWidth > static_cast<std::uint32_t>(width) - x ||
      regionHeight > static_cast<std::uint32_t>(height) - y) {
    return false;
  }
  std::size_t const srcStride =
      sourceBytesPerRow == 0 ? static_cast<std::size_t>(regionWidth) * 4u : sourceBytesPerRow;
  std::size_t const rowBytes = static_cast<std::size_t>(regionWidth) * 4u;
  if (srcStride < rowBytes ||
      newPixels.size() < srcStride * static_cast<std::size_t>(regionHeight - 1u) + rowBytes) {
    return false;
  }
  std::size_t const expectedSize =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
  if (pixels.size() != expectedSize) {
    return false;
  }
  std::size_t const dstStride = static_cast<std::size_t>(width) * 4u;
  for (std::uint32_t row = 0; row < regionHeight; ++row) {
    auto* dst = pixels.data() + (static_cast<std::size_t>(y + row) * dstStride) +
                static_cast<std::size_t>(x) * 4u;
    auto const* src = newPixels.data() + static_cast<std::size_t>(row) * srcStride;
    std::memcpy(dst, src, rowBytes);
  }
  markContentChanged();
  updateVulkanImageTextureRegion(this,
                                 x,
                                 y,
                                 regionWidth,
                                 regionHeight,
                                 newPixels.data(),
                                 static_cast<std::uint32_t>(srcStride));
  return true;
}

} // namespace lambdaui
